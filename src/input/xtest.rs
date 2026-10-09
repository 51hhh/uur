use anyhow::{Context, Result};
use std::collections::HashSet;
use x11rb::connection::Connection;
use x11rb::protocol::xproto::{ConnectionExt, Keycode};
use x11rb::protocol::xtest::ConnectionExt as XtestExt;
use x11rb::rust_connection::RustConnection;

use super::InputBackend;

const FAKE_KEY_PRESS: u8 = 2;
const FAKE_KEY_RELEASE: u8 = 3;
const FAKE_BUTTON_PRESS: u8 = 4;
const FAKE_BUTTON_RELEASE: u8 = 5;
const FAKE_MOTION: u8 = 6;
/// XTEST detail for motion: 0 = absolute, 1 = relative.
const MOTION_RELATIVE: u8 = 1;

/// X11 XTest injection.  Works on every X11 session; on XWayland it reaches
/// XWayland-native windows only, which is why the portal backend is
/// preferred on Wayland-only sessions.
pub struct XtestBackend {
    conn: RustConnection,
    screen: usize,
    held_keys: HashSet<Keycode>,
    held_buttons: HashSet<u8>,
    wheel: super::WheelAccumulator,
}

impl XtestBackend {
    pub fn connect() -> Result<Self> {
        let (conn, screen) = x11rb::connect(None).context("connecting to X display")?;
        Ok(Self {
            conn,
            screen,
            held_keys: HashSet::new(),
            held_buttons: HashSet::new(),
            wheel: super::WheelAccumulator::default(),
        })
    }

    fn root(&self) -> x11rb::protocol::xproto::Window {
        self.conn.setup().roots[self.screen].root
    }

    fn screen_size(&self) -> (i16, i16) {
        let root = &self.conn.setup().roots[self.screen];
        (root.width_in_pixels as i16, root.height_in_pixels as i16)
    }

    fn fake(&self, kind: u8, detail: u8, x: i16, y: i16) -> Result<()> {
        self.conn
            .xtest_fake_input(kind, detail, 0, self.root(), x, y, 0)
            .context("XTestFakeInput")?;
        self.conn.flush()?;
        Ok(())
    }
}

impl InputBackend for XtestBackend {
    fn name(&self) -> &'static str {
        "xtest"
    }

    fn key(&mut self, vkey: u16, down: bool) -> Result<()> {
        let keycode = self.vkey_to_keycode(vkey)?;
        self.fake(
            if down {
                FAKE_KEY_PRESS
            } else {
                FAKE_KEY_RELEASE
            },
            keycode,
            0,
            0,
        )?;
        if down {
            self.held_keys.insert(keycode);
        } else {
            self.held_keys.remove(&keycode);
        }
        Ok(())
    }

    fn button(&mut self, button: u16, down: bool) -> Result<()> {
        let button = match button {
            1 => 1,
            2 => 3, // Windows right button is X11 button 3.
            3 => 2, // Windows middle button is X11 button 2.
            8 => 8,
            9 => 9,
            _ => anyhow::bail!("unsupported pointer button {button}"),
        };
        self.fake(
            if down {
                FAKE_BUTTON_PRESS
            } else {
                FAKE_BUTTON_RELEASE
            },
            button as u8,
            0,
            0,
        )?;
        if down {
            self.held_buttons.insert(button as u8);
        } else {
            self.held_buttons.remove(&(button as u8));
        }
        Ok(())
    }

    fn motion(&mut self, absolute: bool, x: i32, y: i32) -> Result<()> {
        if absolute {
            let (width, height) = self.screen_size();
            let clamped_x = ((x.clamp(0, 65535) as i64 * (width as i64 - 1)) / 65535) as i16;
            let clamped_y = ((y.clamp(0, 65535) as i64 * (height as i64 - 1)) / 65535) as i16;
            self.fake(FAKE_MOTION, 0, clamped_x, clamped_y)?;
        } else {
            self.fake(FAKE_MOTION, MOTION_RELATIVE, x as i16, y as i16)?;
        }
        Ok(())
    }

    fn wheel(&mut self, horizontal: bool, delta: i32) -> Result<()> {
        // X11 has no wheel concept: synthesize button 4/5 (vertical) and
        // 6/7 (horizontal) presses, one per notch.
        let (up_button, down_button) = if horizontal { (7u8, 6u8) } else { (4u8, 5u8) };
        let steps = self.wheel.steps(horizontal, delta);
        for _ in 0..steps.unsigned_abs() {
            let button = if steps > 0 { up_button } else { down_button };
            self.fake(FAKE_BUTTON_PRESS, button, 0, 0)?;
            self.fake(FAKE_BUTTON_RELEASE, button, 0, 0)?;
        }
        Ok(())
    }

    fn release_all(&mut self) -> Result<()> {
        for keycode in self.held_keys.clone() {
            self.fake(FAKE_KEY_RELEASE, keycode, 0, 0)?;
        }
        self.held_keys.clear();
        for button in self.held_buttons.clone() {
            self.fake(FAKE_BUTTON_RELEASE, button, 0, 0)?;
        }
        self.held_buttons.clear();
        self.conn.flush()?;
        Ok(())
    }
}

impl XtestBackend {
    /// Map a Windows virtual-key code onto this server's keymap via the
    /// invariant keysym subset.  Layout-aware remapping for non-representable
    /// keys is Phase 2 together with the semantic text path.
    fn vkey_to_keycode(&self, vkey: u16) -> Result<Keycode> {
        let base = (vkey & 0xff) as u32;
        let extended = vkey & 0x100 != 0;

        let keysym: u32 = match base {
            0x08 => 0xff08,                        // BackSpace
            0x09 => 0xff09,                        // Tab
            0x0d if extended => 0xff8d,            // KP Enter
            0x0d => 0xff0d,                        // Return
            0x10 | 0xa0 => 0xffe1,                 // Shift L
            0x11 | 0xa2 if extended => 0xffe4,     // Control R
            0x11 | 0xa2 => 0xffe3,                 // Control L
            0x12 | 0xa4 if extended => 0xffea,     // Alt R
            0x12 | 0xa4 => 0xffe9,                 // Alt L
            0x13 => 0xff13,                        // Pause
            0x14 => 0xffe5,                        // Caps Lock
            0x1b => 0xff1b,                        // Escape
            0x20 => 0x0020,                        // Space
            0x21 => 0xff55,                        // Prior
            0x22 => 0xff56,                        // Next
            0x23 => 0xff57,                        // End
            0x24 => 0xff50,                        // Home
            0x25 => 0xff51,                        // Left
            0x26 => 0xff52,                        // Up
            0x27 => 0xff53,                        // Right
            0x28 => 0xff54,                        // Down
            0x2d => 0xff63,                        // Insert
            0x2e => 0xffff,                        // Delete
            0x30..=0x39 => base,                   // 0-9
            0x41..=0x5a => base + 0x20,            // A-Z -> lowercase keysyms
            0x60..=0x69 => 0xffb0 + (base - 0x60), // KP_0..KP_9
            0x6a => 0xffaa,                        // KP multiply
            0x6b => 0xffab,                        // KP add
            0x6d => 0xffad,                        // KP subtract
            0x6e => 0xffae,                        // KP decimal
            0x6f => 0xffaf,                        // KP divide
            0x70..=0x87 => 0xffbe + (base - 0x70), // F1..F24
            0xa1 => 0xffe2,                        // Shift R
            0xa3 => 0xffe4,                        // Control R
            0xa5 => 0xffea,                        // Alt R
            0x5b => 0xffeb,                        // Meta L
            0x5c => 0xffec,                        // Meta R
            0xba => 0x3b,                          // ;
            0xbb => 0x3d,                          // =
            0xbc => 0x2c,                          // ,
            0xbd => 0x2d,                          // -
            0xbe => 0x2e,                          // .
            0xbf => 0x2f,                          // /
            0xc0 => 0x60,                          // `
            0xdb => 0x5b,                          // [
            0xdc => 0x5c,                          // backslash
            0xdd => 0x5d,                          // ]
            0xde => 0x27,                          // '
            _ => 0,
        };

        let setup = self.conn.setup();
        let min = setup.min_keycode;
        let max = setup.max_keycode;
        let map = self
            .conn
            .get_keyboard_mapping(min, max - min + 1)?
            .reply()?;

        if keysym != 0 {
            let per = map.keysyms_per_keycode.max(1) as usize;
            for (index, chunk) in map.keysyms.chunks(per).enumerate() {
                if chunk.contains(&keysym) || (keysym == 0xffea && chunk.contains(&0xfe03)) {
                    return Ok(min + index as Keycode);
                }
            }
        }

        // Layout does not represent this key: nearest identity fallback
        // keeps events flowing instead of dropping them silently.
        Ok(base as Keycode)
    }
}

#[cfg(test)]
mod tests {
    use super::XtestBackend;
    use crate::input::InputBackend;
    use x11rb::connection::Connection;
    use x11rb::protocol::xproto::{ChangeWindowAttributesAux, ConnectionExt, EventMask};
    use x11rb::protocol::Event;

    #[test]
    #[ignore = "requires an isolated X server; run with xvfb-run"]
    fn native_x11_buttons_scroll_and_relative_motion() {
        let mut backend = XtestBackend::connect().unwrap();
        let root = backend.root();
        backend
            .conn
            .change_window_attributes(
                root,
                &ChangeWindowAttributesAux::new().event_mask(
                    EventMask::BUTTON_PRESS
                        | EventMask::BUTTON_RELEASE
                        | EventMask::KEY_PRESS
                        | EventMask::KEY_RELEASE,
                ),
            )
            .unwrap()
            .check()
            .unwrap();
        backend.motion(true, 32768, 32768).unwrap();
        let before = backend.conn.query_pointer(root).unwrap().reply().unwrap();
        backend.motion(false, 11, 7).unwrap();
        let after = backend.conn.query_pointer(root).unwrap().reply().unwrap();
        assert_eq!(
            (after.root_x, after.root_y),
            (before.root_x + 11, before.root_y + 7)
        );

        fn next_input_event(backend: &XtestBackend) -> Event {
            loop {
                let event = backend.conn.wait_for_event().unwrap();
                if !matches!(event, Event::MappingNotify(_)) {
                    return event;
                }
            }
        }
        fn button_pair(backend: &XtestBackend, expected: u8) {
            match next_input_event(backend) {
                Event::ButtonPress(event) => assert_eq!(event.detail, expected),
                event => panic!("expected native button press, got {event:?}"),
            }
            match next_input_event(backend) {
                Event::ButtonRelease(event) => assert_eq!(event.detail, expected),
                event => panic!("expected native button release, got {event:?}"),
            }
        }
        for (windows, native) in [(1, 1), (2, 3), (3, 2), (8, 8), (9, 9)] {
            backend.button(windows, true).unwrap();
            backend.button(windows, false).unwrap();
            button_pair(&backend, native);
        }
        for (horizontal, delta, native) in [
            (false, 120, 4),
            (false, -120, 5),
            (true, 120, 7),
            (true, -120, 6),
        ] {
            backend.wheel(horizontal, delta).unwrap();
            button_pair(&backend, native);
        }
        backend.wheel(false, 60).unwrap();
        backend.conn.get_input_focus().unwrap().reply().unwrap();
        assert!(backend.conn.poll_for_event().unwrap().is_none());
        backend.wheel(false, 60).unwrap();
        button_pair(&backend, 4);
        backend
            .conn
            .set_input_focus(
                x11rb::protocol::xproto::InputFocus::POINTER_ROOT,
                root,
                x11rb::CURRENT_TIME,
            )
            .unwrap()
            .check()
            .unwrap();
        for (windows, keysym) in [
            (0x41, 0x61),
            (0x10, 0xffe1),
            (0x11, 0xffe3),
            (0x111, 0xffe4),
            (0x12, 0xffe9),
            (0x10d, 0xff8d),
            (0x2d, 0xff63),
            (0x2e, 0xffff),
        ] {
            backend.key(windows, true).unwrap();
            backend.key(windows, false).unwrap();
            match next_input_event(&backend) {
                Event::KeyPress(event) => {
                    let mapping = backend
                        .conn
                        .get_keyboard_mapping(event.detail, 1)
                        .unwrap()
                        .reply()
                        .unwrap();
                    assert!(
                        mapping.keysyms.contains(&keysym),
                        "wrong native key for Windows {windows:x}"
                    );
                }
                event => panic!("expected native key press, got {event:?}"),
            }
            assert!(matches!(next_input_event(&backend), Event::KeyRelease(_)));
        }
    }
}
