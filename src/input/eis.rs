//! Portal-authorized input using libei. Load the runtime library optionally so
//! older desktops can still use RemoteDesktop's D-Bus input methods.
use anyhow::{anyhow, Context, Result};
use std::collections::HashSet;
use std::ffi::{CStr, CString};
use std::os::fd::{IntoRawFd, OwnedFd};
use std::time::{Duration, Instant};

type Handle = *mut libc::c_void;
const POINTER: i32 = 1;
const ABSOLUTE: i32 = 2;
const KEYBOARD: i32 = 4;
const SCROLL: i32 = 16;
const BUTTON: i32 = 32;

struct Library(Handle);

impl Drop for Library {
    fn drop(&mut self) {
        unsafe { libc::dlclose(self.0) };
    }
}

macro_rules! api {
    ($($name:ident: $ty:ty),+ $(,)?) => {
        pub(super) struct Api {
            $($name: $ty,)+
            _library: Library,
        }
        impl Api {
            pub(super) fn load() -> Result<Self> {
                let handle = unsafe { libc::dlopen(c"libei.so.1".as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL) };
                if handle.is_null() {
                    anyhow::bail!("libei.so.1 is not available");
                }
                let library = Library(handle);
                Ok(Self {
                    $($name: {
                        let name = CString::new(stringify!($name)).unwrap();
                        let symbol = unsafe { libc::dlsym(handle, name.as_ptr()) };
                        if symbol.is_null() {
                            anyhow::bail!("libei is missing {}", stringify!($name));
                        }
                        // These signatures match the public libei 1.x C ABI.
                        unsafe { std::mem::transmute::<Handle, $ty>(symbol) }
                    },)+
                    _library: library,
                })
            }
        }
    };
}

api! {
    ei_new_sender: unsafe extern "C" fn(Handle) -> Handle,
    ei_unref: unsafe extern "C" fn(Handle) -> Handle,
    ei_configure_name: unsafe extern "C" fn(Handle, *const libc::c_char),
    ei_setup_backend_fd: unsafe extern "C" fn(Handle, i32) -> i32,
    ei_dispatch: unsafe extern "C" fn(Handle),
    ei_get_event: unsafe extern "C" fn(Handle) -> Handle,
    ei_event_unref: unsafe extern "C" fn(Handle) -> Handle,
    ei_event_get_type: unsafe extern "C" fn(Handle) -> i32,
    ei_event_get_seat: unsafe extern "C" fn(Handle) -> Handle,
    ei_event_get_device: unsafe extern "C" fn(Handle) -> Handle,
    ei_seat_bind_capabilities: unsafe extern "C" fn(Handle, ...),
    ei_device_ref: unsafe extern "C" fn(Handle) -> Handle,
    ei_device_unref: unsafe extern "C" fn(Handle) -> Handle,
    ei_device_has_capability: unsafe extern "C" fn(Handle, i32) -> bool,
    ei_device_get_region: unsafe extern "C" fn(Handle, usize) -> Handle,
    ei_region_get_x: unsafe extern "C" fn(Handle) -> u32,
    ei_region_get_y: unsafe extern "C" fn(Handle) -> u32,
    ei_region_get_width: unsafe extern "C" fn(Handle) -> u32,
    ei_region_get_height: unsafe extern "C" fn(Handle) -> u32,
    ei_region_get_mapping_id: unsafe extern "C" fn(Handle) -> *const libc::c_char,
    ei_device_start_emulating: unsafe extern "C" fn(Handle, u32),
    ei_device_stop_emulating: unsafe extern "C" fn(Handle),
    ei_device_pointer_motion: unsafe extern "C" fn(Handle, f64, f64),
    ei_device_pointer_motion_absolute: unsafe extern "C" fn(Handle, f64, f64),
    ei_device_button_button: unsafe extern "C" fn(Handle, u32, bool),
    ei_device_keyboard_key: unsafe extern "C" fn(Handle, u32, bool),
    ei_device_scroll_discrete: unsafe extern "C" fn(Handle, i32, i32),
    ei_device_frame: unsafe extern "C" fn(Handle, u64),
    ei_now: unsafe extern "C" fn(Handle) -> u64,
}

struct Device {
    handle: Handle,
    resumed: bool,
    regions: Vec<Region>,
    held_keys: HashSet<i32>,
    held_buttons: HashSet<i32>,
}

#[derive(Debug)]
struct Region {
    mapping_id: Option<String>,
    x: f64,
    y: f64,
    width: f64,
    height: f64,
}

pub(super) struct AbsoluteTarget<'a> {
    pub mapping_id: Option<&'a str>,
    pub fraction_x: f64,
    pub fraction_y: f64,
    pub global_x: f64,
    pub global_y: f64,
}

impl Region {
    fn position(&self, target: &AbsoluteTarget<'_>) -> Option<(f64, f64)> {
        if let (Some(wanted), Some(id)) = (target.mapping_id, self.mapping_id.as_deref()) {
            return (wanted == id).then(|| {
                (
                    self.x + target.fraction_x.clamp(0.0, 1.0) * (self.width - 1.0),
                    self.y + target.fraction_y.clamp(0.0, 1.0) * (self.height - 1.0),
                )
            });
        }
        (target.global_x >= self.x
            && target.global_x < self.x + self.width
            && target.global_y >= self.y
            && target.global_y < self.y + self.height)
            .then_some((target.global_x, target.global_y))
    }
}

pub(super) struct Eis {
    api: Api,
    context: Handle,
    devices: Vec<Device>,
    sequence: u32,
}

impl Eis {
    pub(super) async fn connect(api: Api, fd: OwnedFd) -> Result<Self> {
        let context = unsafe { (api.ei_new_sender)(std::ptr::null_mut()) };
        if context.is_null() {
            anyhow::bail!("creating libei sender failed");
        }
        let mut input = Self {
            api,
            context,
            devices: Vec::new(),
            sequence: 0,
        };
        unsafe { (input.api.ei_configure_name)(context, c"UU Remote".as_ptr()) };
        // libei takes ownership of the private Portal descriptor, including
        // cleanup on failure. Never reopen it or connect to an ambient socket.
        let status = unsafe { (input.api.ei_setup_backend_fd)(context, fd.into_raw_fd()) };
        if status < 0 {
            return Err(std::io::Error::from_raw_os_error(-status)).context("connecting libei");
        }
        let deadline = Instant::now() + Duration::from_secs(3);
        loop {
            input.dispatch()?;
            if input.device(KEYBOARD).is_ok()
                && input.device(ABSOLUTE).is_ok()
                && input.device(BUTTON).is_ok()
                && input.device(POINTER).is_ok()
                && input.device(SCROLL).is_ok()
                && input.devices.iter().any(|device| {
                    device.resumed
                        && !device.regions.is_empty()
                        && unsafe { (input.api.ei_device_has_capability)(device.handle, ABSOLUTE) }
                })
            {
                for device in &input.devices {
                    for region in &device.regions {
                        eprintln!("EIS pointer region: {region:?}");
                    }
                }
                return Ok(input);
            }
            if Instant::now() >= deadline {
                anyhow::bail!(
                    "EIS did not provide ready keyboard, pointer, button, scroll, and absolute-region devices"
                );
            }
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    }

    pub(super) fn dispatch(&mut self) -> Result<()> {
        unsafe { (self.api.ei_dispatch)(self.context) };
        loop {
            let event = unsafe { (self.api.ei_get_event)(self.context) };
            if event.is_null() {
                return Ok(());
            }
            let kind = unsafe { (self.api.ei_event_get_type)(event) };
            let handle = unsafe { (self.api.ei_event_get_device)(event) };
            match kind {
                2 => {
                    unsafe { (self.api.ei_event_unref)(event) };
                    anyhow::bail!("EIS input disconnected");
                }
                3 => {
                    let seat = unsafe { (self.api.ei_event_get_seat)(event) };
                    unsafe {
                        (self.api.ei_seat_bind_capabilities)(
                            seat,
                            POINTER,
                            ABSOLUTE,
                            KEYBOARD,
                            BUTTON,
                            SCROLL,
                            std::ptr::null::<libc::c_void>(),
                        )
                    };
                }
                5 => {
                    unsafe { (self.api.ei_device_ref)(handle) };
                    let mut regions = Vec::new();
                    for index in 0..256 {
                        let region = unsafe { (self.api.ei_device_get_region)(handle, index) };
                        if region.is_null() {
                            break;
                        }
                        let id = unsafe { (self.api.ei_region_get_mapping_id)(region) };
                        let region = Region {
                            mapping_id: (!id.is_null()).then(|| {
                                unsafe { CStr::from_ptr(id) }.to_string_lossy().into_owned()
                            }),
                            x: unsafe { (self.api.ei_region_get_x)(region) } as f64,
                            y: unsafe { (self.api.ei_region_get_y)(region) } as f64,
                            width: unsafe { (self.api.ei_region_get_width)(region) } as f64,
                            height: unsafe { (self.api.ei_region_get_height)(region) } as f64,
                        };
                        if region.width > 0.0 && region.height > 0.0 {
                            regions.push(region);
                        }
                    }
                    self.devices.push(Device {
                        handle,
                        resumed: false,
                        regions,
                        held_keys: HashSet::new(),
                        held_buttons: HashSet::new(),
                    });
                }
                6 => {
                    if let Some(index) = self.devices.iter().position(|d| d.handle == handle) {
                        let device = self.devices.remove(index);
                        unsafe { (self.api.ei_device_unref)(device.handle) };
                    }
                }
                7 | 8 => {
                    if let Some(device) = self.devices.iter_mut().find(|d| d.handle == handle) {
                        device.resumed = kind == 8;
                        if device.resumed {
                            self.sequence = self.sequence.wrapping_add(1);
                            unsafe { (self.api.ei_device_start_emulating)(handle, self.sequence) };
                        }
                    }
                }
                _ => {}
            }
            unsafe { (self.api.ei_event_unref)(event) };
        }
    }

    fn device(&self, capability: i32) -> Result<Handle> {
        self.devices
            .iter()
            .find(|device| {
                device.resumed
                    && unsafe { (self.api.ei_device_has_capability)(device.handle, capability) }
            })
            .map(|device| device.handle)
            .ok_or_else(|| anyhow!("EIS device unavailable for capability {capability}"))
    }

    fn frame(&self, device: Handle) {
        unsafe { (self.api.ei_device_frame)(device, (self.api.ei_now)(self.context)) };
    }

    pub(super) fn absolute(&mut self, target: AbsoluteTarget<'_>) -> Result<()> {
        self.dispatch()?;
        let has_mapping = target.mapping_id.is_some_and(|id| {
            self.devices.iter().any(|device| {
                device.resumed
                    && unsafe { (self.api.ei_device_has_capability)(device.handle, ABSOLUTE) }
                    && device
                        .regions
                        .iter()
                        .any(|region| region.mapping_id.as_deref() == Some(id))
            })
        });
        for device in &self.devices {
            if !device.resumed
                || !unsafe { (self.api.ei_device_has_capability)(device.handle, ABSOLUTE) }
            {
                continue;
            }
            for region in &device.regions {
                if has_mapping && region.mapping_id.as_deref() != target.mapping_id {
                    continue;
                }
                if let Some((x, y)) = region.position(&target) {
                    unsafe { (self.api.ei_device_pointer_motion_absolute)(device.handle, x, y) };
                    self.frame(device.handle);
                    return Ok(());
                }
            }
        }
        anyhow::bail!("EIS has no ready region for the selected screen");
    }

    pub(super) fn relative(&mut self, x: f64, y: f64) -> Result<()> {
        self.dispatch()?;
        let device = self.device(POINTER)?;
        unsafe { (self.api.ei_device_pointer_motion)(device, x, y) };
        self.frame(device);
        Ok(())
    }

    pub(super) fn key(&mut self, code: i32, down: bool) -> Result<()> {
        self.dispatch()?;
        let device = self.device(KEYBOARD)?;
        unsafe { (self.api.ei_device_keyboard_key)(device, code as u32, down) };
        self.frame(device);
        let keys = &mut self
            .devices
            .iter_mut()
            .find(|d| d.handle == device)
            .unwrap()
            .held_keys;
        if down {
            keys.insert(code);
        } else {
            keys.remove(&code);
        }
        Ok(())
    }

    pub(super) fn button(&mut self, code: i32, down: bool) -> Result<()> {
        self.dispatch()?;
        let device = self.device(BUTTON)?;
        unsafe { (self.api.ei_device_button_button)(device, code as u32, down) };
        self.frame(device);
        let buttons = &mut self
            .devices
            .iter_mut()
            .find(|d| d.handle == device)
            .unwrap()
            .held_buttons;
        if down {
            buttons.insert(code);
        } else {
            buttons.remove(&code);
        }
        Ok(())
    }

    pub(super) fn wheel(&mut self, horizontal: bool, delta: i32) -> Result<()> {
        self.dispatch()?;
        let device = self.device(SCROLL)?;
        let (x, y) = if horizontal { (delta, 0) } else { (0, delta) };
        unsafe { (self.api.ei_device_scroll_discrete)(device, x, y) };
        self.frame(device);
        Ok(())
    }
}

impl Drop for Eis {
    fn drop(&mut self) {
        for device in self.devices.drain(..) {
            unsafe {
                if device.resumed {
                    for code in device.held_keys {
                        (self.api.ei_device_keyboard_key)(device.handle, code as u32, false);
                    }
                    for code in device.held_buttons {
                        (self.api.ei_device_button_button)(device.handle, code as u32, false);
                    }
                    (self.api.ei_device_frame)(device.handle, (self.api.ei_now)(self.context));
                    (self.api.ei_device_stop_emulating)(device.handle);
                }
                (self.api.ei_device_unref)(device.handle);
            }
        }
        unsafe { (self.api.ei_unref)(self.context) };
    }
}

#[cfg(test)]
mod tests {
    use super::{AbsoluteTarget, Region};

    #[test]
    fn mapping_id_handles_different_origins_and_scaled_regions() {
        let region = Region {
            mapping_id: Some("external".into()),
            x: 1920.0,
            y: 300.0,
            width: 3840.0,
            height: 2160.0,
        };
        let mut target = AbsoluteTarget {
            mapping_id: Some("external"),
            fraction_x: 0.75,
            fraction_y: 0.9,
            global_x: 0.0,
            global_y: 0.0,
        };
        let (x, y) = region.position(&target).unwrap();
        assert!((x - 4799.25).abs() < 0.000001);
        assert!((y - 2243.1).abs() < 0.000001);
        target.mapping_id = Some("primary");
        assert_eq!(region.position(&target), None);
    }

    #[test]
    fn untagged_regions_use_global_logical_coordinates() {
        let region = Region {
            mapping_id: None,
            x: 2560.0,
            y: 408.0,
            width: 1920.0,
            height: 1200.0,
        };
        let mut target = AbsoluteTarget {
            mapping_id: None,
            fraction_x: 0.0,
            fraction_y: 0.0,
            global_x: 4479.0,
            global_y: 1607.0,
        };
        assert_eq!(region.position(&target), Some((4479.0, 1607.0)));
        target.global_x = 4480.0;
        assert_eq!(region.position(&target), None);
    }
}
