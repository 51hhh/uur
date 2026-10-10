use anyhow::{Context, Result};
use ashpd::zbus::{Connection, Proxy};
use std::sync::mpsc::{self, SyncSender};
use std::time::Duration;

const GNOME_SCREENSAVER_SERVICE: &str = "org.gnome.ScreenSaver";
const GNOME_SCREENSAVER_PATH: &str = "/org/gnome/ScreenSaver";
const GNOME_SCREENSAVER_INTERFACE: &str = "org.gnome.ScreenSaver";
const FREEDESKTOP_SCREENSAVER_SERVICE: &str = "org.freedesktop.ScreenSaver";
const FREEDESKTOP_SCREENSAVER_PATH: &str = "/ScreenSaver";
const FREEDESKTOP_SCREENSAVER_INTERFACE: &str = "org.freedesktop.ScreenSaver";
const MUTTER_DISPLAY_SERVICE: &str = "org.gnome.Mutter.DisplayConfig";
const MUTTER_DISPLAY_PATH: &str = "/org/gnome/Mutter/DisplayConfig";
const MUTTER_DISPLAY_INTERFACE: &str = "org.gnome.Mutter.DisplayConfig";

/// Desktop control that is deliberately separate from the RemoteDesktop
/// portal session.  GNOME closes that session while the screen shield is
/// active, so it cannot be used to wake the display that caused it to close.
pub(super) struct DesktopControl {
    connection: Connection,
}

impl DesktopControl {
    pub(super) async fn connect() -> Result<Self> {
        Ok(Self {
            connection: Connection::session()
                .await
                .context("connecting to the desktop session bus")?,
        })
    }

    async fn screen_saver_active_at(
        &self,
        service: &'static str,
        path: &'static str,
        interface: &'static str,
    ) -> Result<bool> {
        let proxy = Proxy::new(&self.connection, service, path, interface).await?;
        proxy
            .call("GetActive", &())
            .await
            .context("querying the screen saver")
    }

    /// Return `Some(true)` only when a desktop reports an active screen
    /// shield. Unsupported desktops return `None` and retain the old retry
    /// behavior instead of blocking recovery forever.
    pub(super) async fn screen_saver_active(&self) -> Option<bool> {
        if let Ok(active) = self
            .screen_saver_active_at(
                GNOME_SCREENSAVER_SERVICE,
                GNOME_SCREENSAVER_PATH,
                GNOME_SCREENSAVER_INTERFACE,
            )
            .await
        {
            return Some(active);
        }
        self.screen_saver_active_at(
            FREEDESKTOP_SCREENSAVER_SERVICE,
            FREEDESKTOP_SCREENSAVER_PATH,
            FREEDESKTOP_SCREENSAVER_INTERFACE,
        )
        .await
        .ok()
    }

    /// Wake a powered-down display without deactivating or bypassing the lock
    /// screen. Mutter exposes its monitor power state directly; other desktops
    /// can implement the standard ScreenSaver user-activity method.
    async fn wake_display(&self) -> Result<()> {
        if let Ok(proxy) = Proxy::new(
            &self.connection,
            MUTTER_DISPLAY_SERVICE,
            MUTTER_DISPLAY_PATH,
            MUTTER_DISPLAY_INTERFACE,
        )
        .await
        {
            if proxy.set_property("PowerSaveMode", 0i32).await.is_ok() {
                return Ok(());
            }
        }

        let proxy = Proxy::new(
            &self.connection,
            FREEDESKTOP_SCREENSAVER_SERVICE,
            FREEDESKTOP_SCREENSAVER_PATH,
            FREEDESKTOP_SCREENSAVER_INTERFACE,
        )
        .await
        .context("connecting to the desktop wake interface")?;
        let _: () = proxy
            .call("SimulateUserActivity", &())
            .await
            .context("requesting display wake")?;
        Ok(())
    }
}

/// Non-blocking, rate-limited wake requests from the synchronous bridge.
/// Motion can arrive hundreds of times per second, while one monitor power
/// request per second is enough to wake and keep the display on.
pub(super) struct DisplayWake {
    sender: Option<SyncSender<()>>,
}

impl DisplayWake {
    pub(super) fn new() -> Self {
        let (sender, receiver) = mpsc::sync_channel(1);
        let worker = std::thread::Builder::new()
            .name("uur-display-wake".into())
            .spawn(move || {
                let runtime = match tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build()
                {
                    Ok(runtime) => runtime,
                    Err(error) => {
                        eprintln!("display wake unavailable: {error}");
                        return;
                    }
                };
                let mut control = None;
                let mut warned = false;
                while receiver.recv().is_ok() {
                    if control.is_none() {
                        control = runtime.block_on(DesktopControl::connect()).ok();
                    }
                    let result = control.as_ref().map_or_else(
                        || Err(anyhow::anyhow!("desktop session bus is unavailable")),
                        |desktop| runtime.block_on(desktop.wake_display()),
                    );
                    if let Err(error) = result {
                        control = None;
                        if !warned {
                            eprintln!("display wake unavailable: {error}");
                            warned = true;
                        }
                    } else {
                        warned = false;
                    }
                    std::thread::sleep(Duration::from_secs(1));
                    while receiver.try_recv().is_ok() {}
                }
            });
        match worker {
            Ok(_) => Self {
                sender: Some(sender),
            },
            Err(error) => {
                eprintln!("display wake worker unavailable: {error}");
                Self { sender: None }
            }
        }
    }

    pub(super) fn request(&self) {
        if let Some(sender) = &self.sender {
            let _ = sender.try_send(());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::DesktopControl;

    #[test]
    #[ignore = "requires a live graphical session D-Bus"]
    fn active_desktop_reports_lock_state_and_accepts_wake() {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        runtime.block_on(async {
            let desktop = DesktopControl::connect().await.unwrap();
            assert!(desktop.screen_saver_active().await.is_some());
            desktop.wake_display().await.unwrap();
        });
    }
}
