mod eis;
pub mod portal;
pub mod uinput;
pub mod xtest;

use anyhow::Result;

/// A desktop input sink.  Records arrive already normalized by `protocol`.
pub trait InputBackend: Send {
    fn name(&self) -> &'static str;

    fn key(&mut self, vkey: u16, down: bool) -> Result<()>;
    fn button(&mut self, button: u16, down: bool) -> Result<()>;
    /// Absolute (normalized 0..65535) or relative pointer motion.
    fn motion(&mut self, absolute: bool, x: i32, y: i32) -> Result<()>;
    /// Signed Windows units: +120 means up vertically, or right horizontally.
    fn wheel(&mut self, horizontal: bool, delta: i32) -> Result<()>;

    /// Release everything currently held (called when the hook disconnects).
    fn release_all(&mut self) -> Result<()>;
}

/// Choose the backend for this session:
///
/// - Wayland sessions prefer the standard RemoteDesktop portal, then use the
///   kernel-level uinput device when the compositor only provides ScreenCast.
/// - X11 sessions use XTEST, which needs no extra privileges.
pub fn select(preferred: &str) -> Result<Box<dyn InputBackend>> {
    let on_wayland = std::env::var("WAYLAND_DISPLAY").is_ok();

    if preferred == "portal" || (preferred == "auto" && on_wayland) {
        match portal::PortalBackend::connect() {
            Ok(backend) => return Ok(Box::new(backend)),
            Err(error) if preferred == "portal" => return Err(error),
            Err(error) => eprintln!("{}: {error}", t!("input.portal_unavailable")),
        }
    }
    if preferred == "uinput" || (preferred == "auto" && on_wayland) {
        match uinput::UinputBackend::connect() {
            Ok(backend) => return Ok(Box::new(backend)),
            Err(error) if preferred == "uinput" => return Err(error),
            Err(error) => {
                eprintln!("{}: {error}", t!("input.uinput_unavailable"));
                // fall through to XTEST: partial reachability beats nothing
            }
        }
    }
    if std::env::var("DISPLAY").is_ok() {
        return Ok(Box::new(xtest::XtestBackend::connect()?));
    }
    anyhow::bail!("{}", t!("input.no_display"));
}

/// Windows positive vertical input means up; Portal/EIS positive Y means down.
/// Horizontal positive input means right in both interfaces. EIS keeps Windows'
/// 120 units per notch, including fractional notches.
pub(crate) fn portal_wheel_delta(horizontal: bool, delta: i32) -> i32 {
    let delta = delta.clamp(-1920, 1920);
    if horizontal {
        delta
    } else {
        -delta
    }
}

/// Backends accepting only whole notches must retain fractional input instead
/// of turning every small update into a full click. Each axis accumulates alone.
#[derive(Default)]
pub(crate) struct WheelAccumulator {
    horizontal: i32,
    vertical: i32,
}

impl WheelAccumulator {
    pub(crate) fn steps(&mut self, horizontal: bool, delta: i32) -> i32 {
        let remainder = if horizontal {
            &mut self.horizontal
        } else {
            &mut self.vertical
        };
        let total = *remainder + delta.clamp(-1920, 1920);
        *remainder = total % 120;
        total / 120
    }
}

#[cfg(test)]
mod tests {
    use super::{portal_wheel_delta, WheelAccumulator};

    #[test]
    fn portal_scroll_preserves_horizontal_direction_and_reverses_vertical() {
        assert_eq!(portal_wheel_delta(false, 120), -120);
        assert_eq!(portal_wheel_delta(false, -240), 240);
        assert_eq!(portal_wheel_delta(true, 120), 120);
        assert_eq!(portal_wheel_delta(true, -120), -120);
        assert_eq!(portal_wheel_delta(false, 60), -60);
        assert_eq!(portal_wheel_delta(false, 0), 0);
        assert_eq!(portal_wheel_delta(false, i32::MIN), 1920);
    }

    #[test]
    fn fractional_scroll_and_direction_changes_keep_their_total_distance() {
        let mut wheel = WheelAccumulator::default();
        let output = [60, 60, -30, -90, 0].map(|delta| wheel.steps(false, delta));
        assert_eq!(output, [0, 1, 0, -1, 0]);
        assert_eq!(wheel.steps(false, i32::MIN), -16);
        assert_eq!(wheel.steps(false, i32::MAX), 16);
    }

    #[test]
    fn fractional_horizontal_and_vertical_scroll_do_not_mix() {
        let mut wheel = WheelAccumulator::default();
        assert_eq!(wheel.steps(false, 60), 0);
        assert_eq!(wheel.steps(true, 60), 0);
        assert_eq!(wheel.steps(false, 60), 1);
        assert_eq!(wheel.steps(true, -60), 0);
    }
}
