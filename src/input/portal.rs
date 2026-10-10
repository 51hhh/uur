use anyhow::{anyhow, Context, Result};
use ashpd::desktop::remote_desktop::{Axis, DeviceType, KeyState, RemoteDesktop};
use ashpd::desktop::screencast::{CursorMode, Screencast};
use ashpd::desktop::PersistMode;
use std::collections::HashSet;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, SyncSender};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::sync::mpsc::{unbounded_channel, UnboundedReceiver, UnboundedSender};

use super::eis::{AbsoluteTarget, Api, Eis};
use super::InputBackend;

type Reply = SyncSender<std::result::Result<(), String>>;

enum Event {
    Key(i32, bool, Reply),
    Button(i32, bool, Reply),
    FlushMotion,
    Wheel(Axis, i32, Reply),
    Shutdown,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Motion {
    absolute: bool,
    x: f64,
    y: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct PointerTarget {
    stream_id: u32,
    x: f64,
    y: f64,
}

#[derive(Clone, Copy)]
struct StreamRect {
    stream_id: u32,
    x: f64,
    y: f64,
    width: f64,
    height: f64,
}

fn stream_rects(streams: &[crate::capture::PortalStream]) -> Vec<StreamRect> {
    let has_layout = streams.iter().all(|stream| {
        stream.position.is_some()
            && stream
                .size
                .is_some_and(|(width, height)| width > 0 && height > 0)
    });

    if has_layout {
        return streams
            .iter()
            .map(|stream| {
                let (x, y) = stream.position.unwrap();
                let (width, height) = stream.size.unwrap();
                StreamRect {
                    stream_id: stream.node_id,
                    x: x as f64,
                    y: y as f64,
                    width: width as f64,
                    height: height as f64,
                }
            })
            .collect();
    }

    let mut x = 0.0;
    streams
        .iter()
        .map(|stream| {
            let (width, height) = stream.size.unwrap_or((1, 1));
            let rect = StreamRect {
                stream_id: stream.node_id,
                x,
                y: 0.0,
                width: width.max(1) as f64,
                height: height.max(1) as f64,
            };
            x += rect.width;
            rect
        })
        .collect()
}

fn pointer_target(
    streams: &[crate::capture::PortalStream],
    normalized_x: f64,
    normalized_y: f64,
) -> Option<PointerTarget> {
    let rects = stream_rects(streams);
    let min_x = rects.iter().map(|rect| rect.x).reduce(f64::min)?;
    let min_y = rects.iter().map(|rect| rect.y).reduce(f64::min)?;
    let max_x = rects
        .iter()
        .map(|rect| rect.x + rect.width)
        .reduce(f64::max)?;
    let max_y = rects
        .iter()
        .map(|rect| rect.y + rect.height)
        .reduce(f64::max)?;
    let desktop_max_x = (max_x - min_x - 1.0).max(0.0);
    let desktop_max_y = (max_y - min_y - 1.0).max(0.0);
    let global_x = min_x + normalized_x.clamp(0.0, 65535.0) * desktop_max_x / 65535.0;
    let global_y = min_y + normalized_y.clamp(0.0, 65535.0) * desktop_max_y / 65535.0;

    let rect = rects
        .iter()
        .find(|rect| {
            let right = rect.x + rect.width;
            let bottom = rect.y + rect.height;
            global_x >= rect.x
                && (global_x < right || (global_x == max_x && right == max_x))
                && global_y >= rect.y
                && (global_y < bottom || (global_y == max_y && bottom == max_y))
        })
        .or_else(|| {
            rects.iter().min_by(|left, right| {
                let distance = |rect: &&StreamRect| {
                    let dx = if global_x < rect.x {
                        rect.x - global_x
                    } else if global_x > rect.x + rect.width {
                        global_x - rect.x - rect.width
                    } else {
                        0.0
                    };
                    let dy = if global_y < rect.y {
                        rect.y - global_y
                    } else if global_y > rect.y + rect.height {
                        global_y - rect.y - rect.height
                    } else {
                        0.0
                    };
                    dx * dx + dy * dy
                };
                distance(left).total_cmp(&distance(right))
            })
        })?;

    // RemoteDesktop positions are inside a stream, so width/height themselves
    // are outside its accepted range. This matters for normalized 65535 and
    // for points projected out of a gap between differently aligned monitors.
    let max_local_x = (rect.width - 1.0).max(0.0);
    let max_local_y = (rect.height - 1.0).max(0.0);
    Some(PointerTarget {
        stream_id: rect.stream_id,
        x: (global_x - rect.x).clamp(0.0, max_local_x),
        y: (global_y - rect.y).clamp(0.0, max_local_y),
    })
}

fn pointer_in_stream_pixels(
    mut target: PointerTarget,
    logical_size: (i32, i32),
    pixel_size: (u32, u32),
) -> PointerTarget {
    let (logical_width, logical_height) = logical_size;
    let (pixel_width, pixel_height) = pixel_size;
    if logical_width > 0 && logical_height > 0 && pixel_width > 0 && pixel_height > 0 {
        target.x = (target.x * pixel_width as f64 / logical_width as f64)
            .clamp(0.0, (pixel_width - 1) as f64);
        target.y = (target.y * pixel_height as f64 / logical_height as f64)
            .clamp(0.0, (pixel_height - 1) as f64);
    }
    target
}

fn eis_target<'a>(
    streams: &[crate::capture::PortalStream],
    mapping_ids: &'a [Option<String>],
    target: PointerTarget,
) -> Result<AbsoluteTarget<'a>> {
    let rects = stream_rects(streams);
    let index = rects
        .iter()
        .position(|rect| rect.stream_id == target.stream_id)
        .context("absolute input selected an unknown stream")?;
    let rect = rects[index];
    Ok(AbsoluteTarget {
        mapping_id: mapping_ids.get(index).and_then(Option::as_deref),
        fraction_x: target.x / (rect.width - 1.0).max(1.0),
        fraction_y: target.y / (rect.height - 1.0).max(1.0),
        global_x: rect.x + target.x,
        global_y: rect.y + target.y,
    })
}

struct CaptureGuard(std::process::Child);

impl Drop for CaptureGuard {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
        if let Ok(path) = crate::capture::frame_path() {
            crate::capture::remove_frame_files(&path);
        }
    }
}

/// Wayland input through the freedesktop RemoteDesktop portal. The actual
/// backend can be GNOME, KDE, COSMIC, or any future implementation; uur never
/// branches on a desktop name.
pub struct PortalBackend {
    sender: UnboundedSender<Event>,
    pending_motion: Arc<Mutex<Option<Motion>>>,
    motion_queued: Arc<AtomicBool>,
    held_keys: HashSet<i32>,
    held_buttons: HashSet<i32>,
}

impl PortalBackend {
    pub fn connect() -> Result<Self> {
        let mut config = crate::config::Config::load()?;
        let restore_token = config.remote_desktop_restore_token.clone();
        let (sender, receiver) = unbounded_channel();
        let (ready_sender, ready_receiver) = mpsc::sync_channel(1);
        let pending_motion = Arc::new(Mutex::new(None));
        let motion_queued = Arc::new(AtomicBool::new(false));
        let worker_motion = pending_motion.clone();
        let worker_queued = motion_queued.clone();

        std::thread::Builder::new()
            .name("uur-portal-input".into())
            .spawn(move || {
                let result = tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build()
                    .map_err(anyhow::Error::from)
                    .and_then(|runtime| {
                        runtime.block_on(portal_worker(
                            receiver,
                            ready_sender.clone(),
                            restore_token,
                            worker_motion,
                            worker_queued,
                        ))
                    });
                if let Err(error) = result {
                    let _ = ready_sender.send(Err(error.to_string()));
                }
            })
            .context("starting portal input worker")?;

        let fresh_token = ready_receiver
            .recv()
            .context("portal input worker stopped during setup")?
            .map_err(|error| anyhow!(error))?;
        if fresh_token != config.remote_desktop_restore_token {
            config.remote_desktop_restore_token = fresh_token;
            config.store()?;
        }

        Ok(Self {
            sender,
            pending_motion,
            motion_queued,
            held_keys: HashSet::new(),
            held_buttons: HashSet::new(),
        })
    }

    fn submit(&self, event: impl FnOnce(Reply) -> Event) -> Result<()> {
        let (reply_sender, reply_receiver) = mpsc::sync_channel(1);
        self.sender
            .send(event(reply_sender))
            .map_err(|_| anyhow!("portal input session closed"))?;
        reply_receiver
            .recv_timeout(Duration::from_secs(3))
            .context("portal input request timed out")?
            .map_err(|error| anyhow!(error))
    }
}

async fn portal_worker(
    mut receiver: UnboundedReceiver<Event>,
    ready: SyncSender<std::result::Result<Option<String>, String>>,
    restore_token: Option<String>,
    pending_motion: Arc<Mutex<Option<Motion>>>,
    motion_queued: Arc<AtomicBool>,
) -> Result<()> {
    let mut initialized = false;
    let desktop = super::wake::DesktopControl::connect().await.ok();
    let mut waiting_for_unlock = false;
    loop {
        let screen_locked = match &desktop {
            Some(desktop) => desktop.screen_saver_active().await == Some(true),
            None => false,
        };
        if screen_locked {
            if !waiting_for_unlock {
                eprintln!("screen is locked; preserving the Portal grant until unlock");
                waiting_for_unlock = true;
            }
            if discard_pending_events(&mut receiver, &pending_motion, &motion_queued) {
                return Ok(());
            }
            tokio::time::sleep(Duration::from_secs(1)).await;
            continue;
        }
        if waiting_for_unlock {
            eprintln!("screen unlocked; restoring the Portal session");
            waiting_for_unlock = false;
        }
        let token = if initialized {
            crate::config::Config::load()?.remote_desktop_restore_token
        } else {
            restore_token.clone()
        };
        match portal_session_worker(
            &mut receiver,
            &ready,
            token,
            &pending_motion,
            &motion_queued,
            &mut initialized,
        )
        .await
        {
            Ok(()) => return Ok(()),
            Err(error) if !initialized => return Err(error),
            Err(error) => {
                eprintln!("portal session lost: {error}; reconnecting capture and input");
                if discard_pending_events(&mut receiver, &pending_motion, &motion_queued) {
                    return Ok(());
                }
                // Lock activation and Portal teardown race each other. Give the
                // screen shield time to become observable before consuming the
                // single-use restore token in a replacement session.
                tokio::time::sleep(Duration::from_secs(2)).await;
            }
        }
    }
}

fn discard_pending_events(
    receiver: &mut UnboundedReceiver<Event>,
    pending_motion: &Arc<Mutex<Option<Motion>>>,
    motion_queued: &Arc<AtomicBool>,
) -> bool {
    // Commands from a closed session must not be replayed after its
    // replacement is ready, especially queued mouse clicks.
    let mut shutdown = false;
    while let Ok(event) = receiver.try_recv() {
        match event {
            Event::Shutdown => shutdown = true,
            Event::FlushMotion => {}
            Event::Key(_, _, reply) | Event::Button(_, _, reply) | Event::Wheel(_, _, reply) => {
                let _ = reply.send(Err("portal session is reconnecting".into()));
            }
        }
    }
    // Reset after draining FlushMotion events, while holding the producer's
    // pending-motion lock. Otherwise a newly queued event can be drained while
    // its flag remains set forever.
    {
        let mut pending = pending_motion.lock().unwrap();
        pending.take();
        motion_queued.store(false, Ordering::Release);
    }
    shutdown || receiver.is_closed()
}

async fn portal_session_worker(
    receiver: &mut UnboundedReceiver<Event>,
    ready: &SyncSender<std::result::Result<Option<String>, String>>,
    restore_token: Option<String>,
    pending_motion: &Arc<Mutex<Option<Motion>>>,
    motion_queued: &Arc<AtomicBool>,
    initialized: &mut bool,
) -> Result<()> {
    let portal = RemoteDesktop::new().await?;
    let screencast = Screencast::new().await?;
    let configured = crate::config::Config::load()?;
    let source = crate::capture::source_type(&configured.capture_source);
    let available = portal.available_device_types().await?;
    if !available.contains(DeviceType::Keyboard) || !available.contains(DeviceType::Pointer) {
        anyhow::bail!("RemoteDesktop portal does not provide keyboard and pointer devices");
    }

    let session = portal.create_session().await?;
    let result = async {
        portal
            .select_devices(
                &session,
                DeviceType::Keyboard | DeviceType::Pointer,
                restore_token.as_deref(),
                PersistMode::ExplicitlyRevoked,
            )
            .await?
            .response()?;
        screencast
            .select_sources(
                &session,
                CursorMode::Embedded,
                source.into(),
                source == ashpd::desktop::screencast::SourceType::Monitor,
                None,
                PersistMode::DoNot,
            )
            .await?
            .response()?;
        let selection = portal.start(&session, None).await?.response()?;
        // Start consumes the old restore token. Preserve its replacement even if
        // a later capture or EIS setup step fails.
        let token = selection.restore_token().map(ToOwned::to_owned);
        let mut config = crate::config::Config::load()?;
        config.remote_desktop_restore_token = token.clone();
        config.store()?;
        let mapping_ids = selection
            .streams()
            .context("RemoteDesktop portal returned no screen streams")?
            .iter()
            .map(|stream| stream.mapping_id().map(ToOwned::to_owned))
            .collect::<Vec<_>>();
        let streams = selection
            .streams()
            .context("RemoteDesktop portal returned no screen streams")?
            .iter()
            .map(|stream| crate::capture::PortalStream {
                node_id: stream.pipe_wire_node_id(),
                position: stream.position(),
                size: stream.size(),
            })
            .collect::<Vec<_>>();
        let first_stream = streams
            .first()
            .context("RemoteDesktop portal returned no screen stream")?;
        let first_stream_id = first_stream.node_id;
        let mut eis = match Api::load() {
            Ok(api) => match portal.connect_to_eis(&session).await {
                Ok(fd) => match Eis::connect(api, fd).await {
                    Ok(input) => {
                        eprintln!("portal input connected through EIS");
                        Some(input)
                    }
                    Err(error) => {
                        eprintln!("portal EIS setup failed: {error}; using D-Bus input");
                        None
                    }
                },
                Err(error) => {
                    eprintln!("portal EIS unavailable: {error}; using D-Bus input");
                    None
                }
            },
            Err(error) => {
                eprintln!("portal EIS unavailable: {error}; using D-Bus input");
                None
            }
        };
        let pipewire = screencast.open_pipe_wire_remote(&session).await?;
        let mut capture = CaptureGuard(crate::capture::spawn_helper(pipewire, &streams)?);
        let mut pixel_sizes = vec![None; streams.len()];
        if !*initialized {
            ready
                .send(Ok(token))
                .map_err(|_| anyhow!("portal setup receiver closed"))?;
            *initialized = true;
        } else {
            let mut config = crate::config::Config::load()?;
            config.remote_desktop_restore_token = token;
            config.store()?;
            eprintln!("portal capture and input session restored");
        }
        let mut health = tokio::time::interval(Duration::from_millis(500));
        let mut input_health = tokio::time::interval(Duration::from_millis(10));
        input_health.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        let mut session_keys = HashSet::new();
        let mut session_buttons = HashSet::new();
        let mut discrete_wheel = super::WheelAccumulator::default();

        let result = async {
            loop {
                let event = tokio::select! {
                    event = receiver.recv() => match event {
                        Some(event) => event,
                        None => return Ok(()),
                    },
                    _ = health.tick() => {
                        if let Some(status) = capture.0.try_wait()? {
                            anyhow::bail!("PipeWire capture helper exited ({status})");
                        }
                        continue;
                    }
                    _ = input_health.tick(), if eis.is_some() => {
                        eis.as_mut().unwrap().dispatch()?;
                        continue;
                    }
                };
                match event {
                    Event::Key(code, down, reply) => {
                        if !down && !session_keys.remove(&code) {
                            let _ = reply.send(Ok(()));
                            continue;
                        }
                        if down {
                            session_keys.insert(code);
                        }
                        let result = if let Some(input) = eis.as_mut() {
                            input.key(code, down)
                        } else {
                            portal
                                .notify_keyboard_keycode(
                                    &session,
                                    code,
                                    if down {
                                        KeyState::Pressed
                                    } else {
                                        KeyState::Released
                                    },
                                )
                                .await
                                .map_err(anyhow::Error::from)
                        };
                        answer(reply, result)?;
                    }
                    Event::Button(button, down, reply) => {
                        if !down && !session_buttons.remove(&button) {
                            let _ = reply.send(Ok(()));
                            continue;
                        }
                        if down {
                            session_buttons.insert(button);
                        }
                        let result = if let Some(input) = eis.as_mut() {
                            input.button(button, down)
                        } else {
                            portal
                                .notify_pointer_button(
                                    &session,
                                    button,
                                    if down {
                                        KeyState::Pressed
                                    } else {
                                        KeyState::Released
                                    },
                                )
                                .await
                                .map_err(anyhow::Error::from)
                        };
                        answer(reply, result)?;
                    }
                    Event::FlushMotion => loop {
                        let motion = pending_motion.lock().unwrap().take();
                        let Some(motion) = motion else {
                            motion_queued.store(false, Ordering::Release);
                            let has_new_motion = pending_motion.lock().unwrap().is_some();
                            if has_new_motion
                                && motion_queued
                                    .compare_exchange(
                                        false,
                                        true,
                                        Ordering::AcqRel,
                                        Ordering::Acquire,
                                    )
                                    .is_ok()
                            {
                                continue;
                            }
                            break;
                        };
                        let result = if motion.absolute {
                            let mut target = pointer_target(&streams, motion.x, motion.y)
                                .unwrap_or(PointerTarget {
                                    stream_id: first_stream_id,
                                    x: motion.x.clamp(0.0, 65535.0),
                                    y: motion.y.clamp(0.0, 65535.0),
                                });
                            if let Some(input) = eis.as_mut() {
                                input.absolute(eis_target(&streams, &mapping_ids, target)?)
                            } else {
                                if let Some(index) = streams
                                    .iter()
                                    .position(|stream| stream.node_id == target.stream_id)
                                {
                                    let pixel_size = *pixel_sizes[index].get_or_insert_with(|| {
                                        crate::capture::frame_pixel_size(index).unwrap_or((0, 0))
                                    });
                                    if pixel_size == (0, 0) {
                                        pixel_sizes[index] = None;
                                    } else if let Some(size) = streams[index].size {
                                        target = pointer_in_stream_pixels(target, size, pixel_size);
                                    }
                                }
                                portal
                                    .notify_pointer_motion_absolute(
                                        &session,
                                        target.stream_id,
                                        target.x,
                                        target.y,
                                    )
                                    .await
                                    .map_err(anyhow::Error::from)
                            }
                        } else {
                            if let Some(input) = eis.as_mut() {
                                input.relative(motion.x, motion.y)
                            } else {
                                portal
                                    .notify_pointer_motion(&session, motion.x, motion.y)
                                    .await
                                    .map_err(anyhow::Error::from)
                            }
                        };
                        if let Err(error) = result {
                            eprintln!("portal pointer motion failed: {error}");
                            let message = error.to_string();
                            if eis.is_some()
                                || message.contains("Invalid session")
                                || message.contains("Session not started")
                            {
                                return Err(error);
                            }
                        }
                    },
                    Event::Wheel(axis, delta, reply) => {
                        let horizontal = matches!(axis, Axis::Horizontal);
                        let result = if let Some(input) = eis.as_mut() {
                            input.wheel(horizontal, delta)
                        } else {
                            let steps = discrete_wheel.steps(horizontal, delta);
                            if steps == 0 {
                                Ok(())
                            } else {
                                portal
                                    .notify_pointer_axis_discrete(&session, axis, steps)
                                    .await
                                    .map_err(anyhow::Error::from)
                            }
                        };
                        answer(reply, result)?;
                    }
                    Event::Shutdown => break,
                }
            }
            Ok(())
        }
        .await;
        result
    }
    .await;
    // A helper failure does not necessarily close the Portal session. Close
    // it explicitly before negotiating a replacement with the saved grant.
    let _ = session.close().await;
    result
}

fn answer<T>(reply: Reply, result: Result<T>) -> Result<()> {
    let failure = result.as_ref().err().map(ToString::to_string);
    let _ = reply.send(result.map(|_| ()).map_err(|error| error.to_string()));
    failure.map_or(Ok(()), |error| Err(anyhow!(error)))
}

impl InputBackend for PortalBackend {
    fn name(&self) -> &'static str {
        "portal"
    }

    fn key(&mut self, vkey: u16, down: bool) -> Result<()> {
        let code = super::uinput::vk_to_linux_key(vkey) as i32;
        if code == 0 {
            anyhow::bail!("Windows virtual key 0x{vkey:02x} has no evdev mapping");
        }
        self.submit(|reply| Event::Key(code, down, reply))?;
        if down {
            self.held_keys.insert(code);
        } else {
            self.held_keys.remove(&code);
        }
        Ok(())
    }

    fn button(&mut self, button: u16, down: bool) -> Result<()> {
        let evdev = match button {
            1 => input_linux_sys::BTN_LEFT,
            2 => input_linux_sys::BTN_RIGHT,
            3 => input_linux_sys::BTN_MIDDLE,
            8 => input_linux_sys::BTN_SIDE,
            9 => input_linux_sys::BTN_EXTRA,
            _ => anyhow::bail!("unsupported pointer button {button}"),
        } as i32;
        self.submit(|reply| Event::Button(evdev, down, reply))?;
        if down {
            self.held_buttons.insert(evdev);
        } else {
            self.held_buttons.remove(&evdev);
        }
        Ok(())
    }

    fn motion(&mut self, absolute: bool, x: i32, y: i32) -> Result<()> {
        let mut pending = self.pending_motion.lock().unwrap();
        merge_motion(
            &mut pending,
            Motion {
                absolute,
                x: x as f64,
                y: y as f64,
            },
        );
        drop(pending);
        if !self.motion_queued.swap(true, Ordering::AcqRel)
            && self.sender.send(Event::FlushMotion).is_err()
        {
            self.motion_queued.store(false, Ordering::Release);
            anyhow::bail!("portal input session closed");
        }
        Ok(())
    }

    fn wheel(&mut self, horizontal: bool, delta: i32) -> Result<()> {
        let axis = if horizontal {
            Axis::Horizontal
        } else {
            Axis::Vertical
        };
        let delta = super::portal_wheel_delta(horizontal, delta);
        self.submit(|reply| Event::Wheel(axis, delta, reply))
    }

    fn release_all(&mut self) -> Result<()> {
        self.pending_motion.lock().unwrap().take();
        for key in self.held_keys.clone() {
            let _ = self.submit(|reply| Event::Key(key, false, reply));
        }
        self.held_keys.clear();
        for button in self.held_buttons.clone() {
            let _ = self.submit(|reply| Event::Button(button, false, reply));
        }
        self.held_buttons.clear();
        Ok(())
    }
}

fn merge_motion(pending: &mut Option<Motion>, next: Motion) {
    if !next.absolute {
        if let Some(current) = pending.as_mut().filter(|motion| !motion.absolute) {
            current.x += next.x;
            current.y += next.y;
            return;
        }
    }
    *pending = Some(next);
}

impl Drop for PortalBackend {
    fn drop(&mut self) {
        let _ = self.release_all();
        let _ = self.sender.send(Event::Shutdown);
    }
}

#[cfg(test)]
mod tests {
    use super::{merge_motion, pointer_in_stream_pixels, pointer_target, Motion, PointerTarget};
    use crate::capture::PortalStream;

    #[test]
    fn absolute_motion_keeps_only_the_latest_position() {
        let mut pending = Some(Motion {
            absolute: true,
            x: 10.0,
            y: 20.0,
        });
        let latest = Motion {
            absolute: true,
            x: 30.0,
            y: 40.0,
        };
        merge_motion(&mut pending, latest);
        assert_eq!(pending, Some(latest));
    }

    #[test]
    fn relative_motion_accumulates_without_building_a_queue() {
        let mut pending = Some(Motion {
            absolute: false,
            x: 3.0,
            y: -2.0,
        });
        merge_motion(
            &mut pending,
            Motion {
                absolute: false,
                x: 4.0,
                y: 5.0,
            },
        );
        assert_eq!(
            pending,
            Some(Motion {
                absolute: false,
                x: 7.0,
                y: 3.0,
            })
        );
    }

    #[test]
    fn absolute_motion_selects_the_matching_monitor() {
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

        let left = pointer_target(&streams, 0.0, 0.0).unwrap();
        assert_eq!(left.stream_id, 17);
        assert_eq!(left.x, 0.0);

        let right = pointer_target(&streams, 65535.0, 65535.0).unwrap();
        assert_eq!(
            right,
            PointerTarget {
                stream_id: 23,
                x: 2559.0,
                y: 1599.0,
            }
        );

        let boundary_coordinate = (1920.0_f64 / 4479.0 * 65535.0).ceil();
        let boundary = pointer_target(&streams, boundary_coordinate, 0.0).unwrap();
        assert_eq!(boundary.stream_id, 23);
        assert!(boundary.x < 1.0);
    }

    #[test]
    fn streams_without_positions_are_arranged_horizontally() {
        let streams = [
            PortalStream {
                node_id: 1,
                position: None,
                size: Some((100, 100)),
            },
            PortalStream {
                node_id: 2,
                position: None,
                size: Some((100, 100)),
            },
        ];
        assert_eq!(pointer_target(&streams, 65535.0, 0.0).unwrap().stream_id, 2);
    }

    #[test]
    fn absolute_input_uses_stream_pixels_on_fractionally_scaled_monitors() {
        let primary = pointer_in_stream_pixels(
            PointerTarget {
                stream_id: 23,
                x: 960.0,
                y: 600.0,
            },
            (1920, 1200),
            (2560, 1600),
        );
        assert_eq!(primary.x, 1280.0);
        assert_eq!(primary.y, 800.0);
        let secondary = pointer_in_stream_pixels(
            PointerTarget {
                stream_id: 17,
                x: 1280.0,
                y: 720.0,
            },
            (2560, 1440),
            (3840, 2160),
        );
        assert_eq!(secondary.x, 1920.0);
        assert_eq!(secondary.y, 1080.0);
    }
}
