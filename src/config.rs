use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

static CONFIG_TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

/// Persistent user configuration.  Everything lives under the XDG state and
/// config directories; the managed Wine prefix lives under data.
#[derive(Serialize, Deserialize, Clone)]
#[serde(default)]
pub struct Config {
    /// EULA acceptance recorded during `uur setup`.
    pub eula_accepted: bool,
    /// Loopback port the in-client hook connects to.
    pub bridge_port: u16,
    /// Random per-install token the hook must present on HELLO.
    pub bridge_token: String,
    /// Preferred input backend: "auto" | "xtest" | "portal".
    pub input_backend: String,
    /// Release channel pinned for provisioning.
    pub pinned_version: Option<String>,
    /// ScreenCast portal restore token: after the first consent the portal
    /// restores the monitor selection silently.
    pub capture_restore_token: Option<String>,
    /// Persistent permission for the standard RemoteDesktop input portal.
    pub remote_desktop_restore_token: Option<String>,
    /// Portal source type: monitor, window, or virtual.
    pub capture_source: String,
    /// Let an authenticated remote input event dismiss the GNOME screen
    /// shield. Disabled by default because this bypasses the local lock prompt.
    pub unlock_on_remote_input: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            eula_accepted: false,
            bridge_port: 47010,
            bridge_token: generate_token(),
            input_backend: "auto".into(),
            pinned_version: None,
            capture_restore_token: None,
            remote_desktop_restore_token: None,
            capture_source: "monitor".into(),
            unlock_on_remote_input: false,
        }
    }
}

fn generate_token() -> String {
    let mut bytes = [0u8; 32];
    // Read EXACTLY 32 bytes.  Never `fs::read` a character device like
    // /dev/urandom: it has no EOF, so fs::read would loop forever and
    // consume all memory (this actually caused an OOM once).
    let filled = std::fs::OpenOptions::new()
        .read(true)
        .open("/dev/urandom")
        .and_then(|mut f| {
            use std::io::Read;
            f.read_exact(&mut bytes)
        })
        .is_ok();
    if !filled {
        // No OS entropy available: derive a weak token from the clock so
        // the tool still functions on loopback.  Logged as a condition,
        // not a crash.
        let t = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let mut state = t as u64 | 1;
        for byte in bytes.iter_mut() {
            // xorshift64 — not cryptographic, only a stopgap.
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            *byte = state as u8;
        }
    }
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn config_path() -> Result<PathBuf> {
    let base = std::env::var("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|_| std::env::var("HOME").map(|h| PathBuf::from(h).join(".config")))
        .context("cannot determine config directory")?;
    Ok(base.join("uur").join("config.toml"))
}

pub fn data_dir() -> Result<PathBuf> {
    let base = std::env::var("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|_| std::env::var("HOME").map(|h| PathBuf::from(h).join(".local/share")))
        .context("cannot determine data directory")?;
    Ok(base.join("uur"))
}

/// Private per-login runtime state. Capture frames are sensitive and must not
/// live in a world-readable global /dev/shm name shared by every user.
pub fn runtime_dir() -> Result<PathBuf> {
    let base = std::env::var("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(format!("/tmp/uur-{}", unsafe { libc::geteuid() })));
    let dir = base.join("uur");
    std::fs::create_dir_all(&dir)
        .with_context(|| format!("creating runtime directory {}", dir.display()))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700))?;
    }
    Ok(dir)
}

impl Config {
    pub fn load() -> Result<Self> {
        let path = config_path()?;
        if !path.exists() {
            let config = Self::default();
            config.store()?;
            return Ok(config);
        }
        secure_config_path(&path)?;
        let raw = std::fs::read_to_string(&path)
            .with_context(|| format!("reading {}", path.display()))?;
        toml::from_str(&raw).context("parsing config.toml")
    }

    pub fn store(&self) -> Result<()> {
        let path = config_path()?;
        self.store_at(&path)
    }

    fn store_at(&self, path: &Path) -> Result<()> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)
                .with_context(|| format!("creating {}", parent.display()))?;
            set_private_mode(parent, 0o700)?;
        }
        let parent = path.parent().unwrap_or_else(|| Path::new("."));
        let name = path
            .file_name()
            .context("configuration path has no file name")?
            .to_string_lossy();
        let contents = toml::to_string_pretty(self)?;

        let mut temporary = None;
        for _ in 0..32 {
            let sequence = CONFIG_TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let temporary_path =
                parent.join(format!(".{name}.uur-{}-{sequence}.tmp", std::process::id()));
            let mut options = std::fs::OpenOptions::new();
            options.write(true).create_new(true);
            #[cfg(unix)]
            {
                use std::os::unix::fs::OpenOptionsExt;
                options.mode(0o600);
            }
            match options.open(&temporary_path) {
                Ok(file) => {
                    temporary = Some((temporary_path, file));
                    break;
                }
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => {
                    return Err(error).with_context(|| {
                        format!("creating temporary configuration in {}", parent.display())
                    });
                }
            }
        }
        let Some((temporary_path, mut file)) = temporary else {
            anyhow::bail!("could not create a temporary configuration file");
        };

        let result = (|| -> Result<()> {
            file.write_all(contents.as_bytes())?;
            file.sync_all()?;
            drop(file);
            std::fs::rename(&temporary_path, path)
                .with_context(|| format!("installing {}", path.display()))?;
            set_private_mode(path, 0o600)?;
            Ok(())
        })();
        if result.is_err() {
            let _ = std::fs::remove_file(&temporary_path);
        }
        result
    }
}

fn secure_config_path(path: &Path) -> Result<()> {
    if let Some(parent) = path.parent() {
        set_private_mode(parent, 0o700)?;
    }
    set_private_mode(path, 0o600)
}

#[cfg(unix)]
fn set_private_mode(path: &Path, mode: u32) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode))
        .with_context(|| format!("securing {}", path.display()))
}

#[cfg(not(unix))]
fn set_private_mode(_path: &Path, _mode: u32) -> Result<()> {
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;

    #[test]
    fn token_generation_terminates_and_is_wellformed() {
        // Regression: generate_token once used fs::read on /dev/urandom,
        // which never returns EOF and consumed all memory.
        let token = generate_token();
        assert_eq!(token.len(), 64);
        assert!(token.chars().all(|c| c.is_ascii_hexdigit()));
    }

    #[test]
    fn older_configuration_gets_new_defaults() {
        let config: Config = toml::from_str(
            r#"
eula_accepted = true
bridge_port = 47010
bridge_token = "00"
input_backend = "auto"
"#,
        )
        .unwrap();
        assert!(config.remote_desktop_restore_token.is_none());
        assert!(config.capture_restore_token.is_none());
        assert_eq!(config.capture_source, "monitor");
        assert!(!config.unlock_on_remote_input);
    }

    #[cfg(unix)]
    #[test]
    fn stored_configuration_is_private() {
        let directory = std::env::temp_dir().join(format!(
            "uur-config-test-{}-{}",
            std::process::id(),
            CONFIG_TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        let path = directory.join("config.toml");
        let config = Config::default();
        config.store_at(&path).unwrap();

        assert_eq!(
            std::fs::metadata(&directory).unwrap().permissions().mode() & 0o777,
            0o700
        );
        assert_eq!(
            std::fs::metadata(&path).unwrap().permissions().mode() & 0o777,
            0o600
        );
        let restored: Config = toml::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
        assert_eq!(restored.bridge_token, config.bridge_token);
        let _ = std::fs::remove_dir_all(directory);
    }
}
