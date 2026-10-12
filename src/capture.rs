//! Portal side of the capture path.
//!
//! Negotiates an xdg-desktop-portal ScreenCast session, obtains a private
//! PipeWire file descriptor and the selected video nodes, then hands them to the
//! C capture helper (`uur-pw-capture`) which runs the PipeWire stream and
//! publishes each monitor through an independent private XDG runtime file (see
//! docs/capture-protocol.md).
//!
//! Consent: the first run shows the compositor's share dialog once.  The
//! returned restore token is stored in uur's config; every later run hands
//! it back with PersistMode::Persistent and the portal restores the choice
//! silently — no dialog, no local interaction, which is the whole point on
//! a headless remote host.

use anyhow::{anyhow, Context, Result};
use std::io::Read;
use std::os::fd::AsRawFd;
use std::os::fd::OwnedFd;
use std::path::PathBuf;
use std::process::{Child, Command};

use crate::config::Config;

pub(crate) const MAX_PORTAL_STREAMS: usize = 16;

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PortalStream {
    pub(crate) node_id: u32,
    pub(crate) position: Option<(i32, i32)>,
    pub(crate) size: Option<(i32, i32)>,
}

impl PortalStream {
    fn from_portal(stream: &ashpd::desktop::screencast::Stream) -> Self {
        Self {
            node_id: stream.pipe_wire_node_id(),
            position: stream.position(),
            size: stream.size(),
        }
    }
}

/// Run the capture path until interrupted.
pub fn run() -> Result<()> {
    let mut config = Config::load()?;
    let (pw_fd, streams) = portal_session(&mut config)?;
    let nodes = streams
        .iter()
        .map(|stream| stream.node_id.to_string())
        .collect::<Vec<_>>()
        .join(",");
    println!("{}", t!("capture.session_ready", node = nodes));

    let mut helper = spawn_helper(pw_fd, &streams)?;
    let status = helper.wait().context("waiting for capture helper")?;
    if !status.success() {
        anyhow::bail!("{}", t!("capture.helper_failed"));
    }
    Ok(())
}

/// Start the native PipeWire consumer with a portal-owned descriptor. The
/// descriptor remains private to the child and is never reopened by path.
pub(crate) fn spawn_helper(pw_fd: OwnedFd, streams: &[PortalStream]) -> Result<Child> {
    if streams.is_empty() {
        anyhow::bail!("portal returned no capture streams");
    }
    if streams.len() > MAX_PORTAL_STREAMS {
        anyhow::bail!("portal returned too many capture streams");
    }
    // The helper inherits this descriptor: strip close-on-exec and keep the
    // OwnedFd alive across the spawn (closing it would invalidate the number
    // the child receives).
    let raw = pw_fd.as_raw_fd();
    unsafe {
        let flags = libc::fcntl(raw, libc::F_GETFD);
        if flags < 0 {
            return Err(std::io::Error::last_os_error()).context("fcntl F_GETFD");
        }
        if libc::fcntl(raw, libc::F_SETFD, flags & !libc::FD_CLOEXEC) < 0 {
            return Err(std::io::Error::last_os_error()).context("fcntl F_SETFD");
        }
    }

    let helper = find_helper()?;
    let frames = frame_path()?;
    // A previous helper may have been killed before it could unlink the
    // transport. Remove it before spawning so a still-running Wine process
    // cannot attach to an obsolete inode while the portal is negotiating.
    remove_frame_files(&frames);
    let mut command = Command::new(&helper);
    command
        .env("UUR_PW_FD", raw.to_string())
        .env("UUR_PW_STREAMS", encode_streams(streams))
        .env("UUR_FRAME_PATH", frames);
    if let Ok(debug) = std::env::var("UUR_PW_DEBUG") {
        command.env("PIPEWIRE_DEBUG", debug);
    }
    let child = command
        .spawn()
        .with_context(|| format!("running {}", helper.display()))?;
    Ok(child)
}

pub(crate) fn frame_path() -> Result<PathBuf> {
    if let Some(path) = std::env::var_os("UUR_FRAME_PATH") {
        return Ok(PathBuf::from(path));
    }
    Ok(crate::config::runtime_dir()?.join("frames.v1"))
}

/// Pixel dimensions negotiated by PipeWire, rather than Portal's compositor
/// layout dimensions. D-Bus absolute input is relative to the video stream.
pub(crate) fn frame_pixel_size(index: usize) -> Option<(u32, u32)> {
    if index >= MAX_PORTAL_STREAMS {
        return None;
    }
    let base = frame_path().ok()?;
    let path = if index == 0 {
        base
    } else {
        PathBuf::from(format!("{}.{index}", base.display()))
    };
    let mut header = [0u8; 64];
    std::fs::File::open(path)
        .ok()?
        .read_exact(&mut header)
        .ok()?;
    let field = |offset| u32::from_le_bytes(header[offset..offset + 4].try_into().unwrap());
    let width = field(8);
    let height = field(12);
    if field(0) != 0x46525555
        || field(4) != 2
        || field(48) != index as u32
        || width == 0
        || height == 0
        || width > 32768
        || height > 32768
    {
        return None;
    }
    Some((width, height))
}

pub(crate) fn remove_frame_files(base: &std::path::Path) {
    for index in 0..MAX_PORTAL_STREAMS {
        let path = if index == 0 {
            base.to_path_buf()
        } else {
            PathBuf::from(format!("{}.{index}", base.display()))
        };
        let _ = std::fs::remove_file(path);
    }
}

/// Open the portal session and return its PipeWire descriptor and streams.
/// Reuse the stored restore token and let the Portal decide whether it is
/// still valid. The specification requires an invalid token to be ignored and
/// the chooser to be shown normally; retrying every transient failure without
/// the token would turn a lock-screen interruption into repeated prompts.
#[allow(clippy::type_complexity)]
fn portal_session(config: &mut Config) -> Result<(OwnedFd, Vec<PortalStream>)> {
    // Restore tokens belong to the Portal flow that issued them. A combined
    // RemoteDesktop grant cannot be reused by this independent ScreenCast
    // session, even though both flows eventually expose PipeWire streams.
    let saved_token = config.capture_restore_token.clone();
    let runtime = tokio::runtime::Runtime::new().context("creating tokio runtime")?;
    runtime.block_on(async move {
        let screencast = ashpd::desktop::screencast::Screencast::new()
            .await
            .context("connecting to the ScreenCast portal")?;

        let source = source_type(&config.capture_source);
        let (fd, streams, fresh_token) =
            negotiate(&screencast, source, saved_token.as_deref()).await?;
        if fresh_token != config.capture_restore_token {
            config.capture_restore_token = fresh_token;
            config.store().context("storing the new restore token")?;
        }
        Ok((fd, streams))
    })
}

/// One full portal handshake.  Returns the PipeWire fd, the video node id
/// and the restore token the portal issued for this selection.
async fn negotiate(
    screencast: &ashpd::desktop::screencast::Screencast<'_>,
    source: ashpd::desktop::screencast::SourceType,
    restore_token: Option<&str>,
) -> Result<(OwnedFd, Vec<PortalStream>, Option<String>)> {
    use ashpd::desktop::screencast::CursorMode;
    use ashpd::desktop::PersistMode;

    let session = screencast.create_session().await?;

    screencast
        .select_sources(
            &session,
            // Mutter's embedded cursor follows the compositor seat. Absolute
            // Xwayland warps update Win32 hit testing without moving that
            // hardware cursor, so the hook composites Wine's current cursor
            // after copying each Portal frame.
            CursorMode::Hidden,
            source.into(),
            source == ashpd::desktop::screencast::SourceType::Monitor,
            restore_token,
            PersistMode::ExplicitlyRevoked,
        )
        .await?;

    let response = screencast.start(&session, None).await?;
    let streams_response = response.response()?;
    let streams = streams_response
        .streams()
        .iter()
        .map(PortalStream::from_portal)
        .collect::<Vec<_>>();
    if streams.is_empty() {
        anyhow::bail!("no monitor selected");
    }
    let fresh_token = streams_response.restore_token().map(String::from);

    let fd = screencast
        .open_pipe_wire_remote(&session)
        .await
        .context("opening the PipeWire remote")?;
    Ok((fd, streams, fresh_token))
}

fn encode_streams(streams: &[PortalStream]) -> String {
    streams
        .iter()
        .map(|stream| {
            let (x, y) = stream.position.unwrap_or((0, 0));
            let (width, height) = stream.size.unwrap_or((0, 0));
            format!("{},{},{},{},{}", stream.node_id, x, y, width, height)
        })
        .collect::<Vec<_>>()
        .join(";")
}

pub(crate) fn source_type(value: &str) -> ashpd::desktop::screencast::SourceType {
    use ashpd::desktop::screencast::SourceType;
    match value {
        "window" => SourceType::Window,
        "virtual" => SourceType::Virtual,
        _ => SourceType::Monitor,
    }
}

fn find_helper() -> Result<PathBuf> {
    let mut candidates = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        // Prefer components installed beside the running executable. This
        // prevents a ~/.local binary from accidentally loading an older
        // system-wide helper from /usr/lib.
        if let Some(dir) = exe.parent() {
            candidates.push(dir.join("../lib/uur/uur-pw-capture"));
            candidates.push(dir.join("../../capture/uur-pw-capture"));
            candidates.push(dir.join("uur-pw-capture"));
        }
    }
    if let Ok(cwd) = std::env::current_dir() {
        candidates.push(cwd.join("capture/uur-pw-capture"));
    }
    if let Ok(home) = std::env::var("HOME") {
        candidates.push(PathBuf::from(home).join(".local/lib/uur/uur-pw-capture"));
    }
    candidates.push(PathBuf::from("/usr/local/lib/uur/uur-pw-capture"));
    candidates.push(PathBuf::from("/usr/lib/uur/uur-pw-capture"));
    candidates.into_iter().find(|p| p.is_file()).ok_or_else(|| {
        anyhow!("capture helper uur-pw-capture not found (build with capture/build.sh)")
    })
}

#[cfg(test)]
mod tests {
    use super::{encode_streams, source_type, PortalStream};
    use ashpd::desktop::screencast::SourceType;

    #[test]
    fn configured_portal_source_is_mapped() {
        assert_eq!(source_type("monitor"), SourceType::Monitor);
        assert_eq!(source_type("window"), SourceType::Window);
        assert_eq!(source_type("virtual"), SourceType::Virtual);
    }

    #[test]
    fn multiple_streams_keep_their_compositor_geometry() {
        let streams = [
            PortalStream {
                node_id: 17,
                position: Some((-1920, 0)),
                size: Some((1920, 1080)),
            },
            PortalStream {
                node_id: 23,
                position: Some((0, 0)),
                size: Some((2560, 1600)),
            },
        ];
        assert_eq!(
            encode_streams(&streams),
            "17,-1920,0,1920,1080;23,0,0,2560,1600"
        );
    }
}
