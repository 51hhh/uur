//! Host wired Wake-on-LAN and Wi-Fi WoWLAN configuration.

use anyhow::{Context, Result};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

#[derive(Debug, Clone)]
pub struct State {
    pub interface: String,
    pub mac: String,
    pub supported: Option<bool>,
    pub enabled: Option<bool>,
    pub connection: Option<String>,
}

pub fn status(requested: Option<&str>) -> Result<()> {
    let state = inspect(requested)?;
    println!("{}", t!("wol.interface", interface = state.interface));
    println!("{}", t!("wol.mac", mac = state.mac));
    println!(
        "{}",
        t!("wol.magic_support", state = tri_state(state.supported))
    );
    println!(
        "{}",
        t!("wol.magic_enabled", state = tri_state(state.enabled))
    );
    println!(
        "{}",
        t!(
            "wol.connection",
            connection = state.connection.as_deref().unwrap_or("-")
        )
    );
    Ok(())
}

pub fn configure(requested: Option<&str>, enabled: bool) -> Result<()> {
    let before = inspect(requested)?;
    if enabled && before.supported == Some(false) {
        anyhow::bail!("{}", t!("wol.unsupported", interface = before.interface));
    }

    let wireless = is_wireless_physical(&before.interface);
    if let Some(connection) = &before.connection {
        let value = if enabled { "magic" } else { "0" };
        let property = if wireless {
            "802-11-wireless.wake-on-wlan"
        } else {
            "802-3-ethernet.wake-on-lan"
        };
        checked(
            Command::new("nmcli")
                .args(["connection", "modify", connection, property, value])
                .stdin(Stdio::null()),
            "updating the NetworkManager Wake-on-LAN profile",
        )?;
        if wireless {
            checked(
                Command::new("nmcli")
                    .args(["device", "reapply", &before.interface])
                    .stdin(Stdio::null()),
                "applying the NetworkManager WoWLAN profile",
            )?;
        }
    }

    if wireless {
        if before.connection.is_none() {
            configure_wireless_runtime(&before.interface, enabled)?;
        }
    } else {
        let mode = if enabled { "g" } else { "d" };
        checked(
            Command::new("ethtool")
                .args(["-s", &before.interface, "wol", mode])
                .stdin(Stdio::null()),
            &format!(
                "setting Wake-on-LAN on {} (run this command as root or through an authorized Polkit session)",
                before.interface
            ),
        )?;
    }

    let after = inspect(Some(&before.interface))?;
    if after.enabled != Some(enabled) {
        anyhow::bail!("{}", t!("wol.verify_failed", interface = before.interface));
    }
    println!(
        "{}",
        if enabled {
            t!("wol.enabled", interface = before.interface)
        } else {
            t!("wol.disabled", interface = before.interface)
        }
    );
    Ok(())
}

pub fn inspect(requested: Option<&str>) -> Result<State> {
    let interface = select_interface(requested)?;
    let base = Path::new("/sys/class/net").join(&interface);
    let mac = std::fs::read_to_string(base.join("address"))
        .unwrap_or_else(|_| "unknown".into())
        .trim()
        .to_string();
    let (supported, enabled) = if is_wireless_physical(&interface) {
        inspect_wireless(&interface)
    } else {
        inspect_wired(&interface)
    };
    Ok(State {
        interface: interface.clone(),
        mac,
        supported,
        enabled,
        connection: active_connection(&interface),
    })
}

fn select_interface(requested: Option<&str>) -> Result<String> {
    if let Some(interface) = requested {
        validate_interface(interface)?;
        return Ok(interface.to_string());
    }
    if let Some(interface) = default_route_interface().filter(|name| is_physical(name)) {
        return Ok(interface);
    }
    let mut candidates = std::fs::read_dir("/sys/class/net")
        .context("enumerating network interfaces")?
        .flatten()
        .filter_map(|entry| entry.file_name().into_string().ok())
        .filter(|name| is_physical(name))
        .collect::<Vec<_>>();
    candidates.sort_by_key(|name| (is_wireless_physical(name), name.clone()));
    candidates
        .into_iter()
        .next()
        .context("no physical network interface found")
}

fn validate_interface(interface: &str) -> Result<()> {
    if interface.is_empty()
        || !interface
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.' | b':'))
        || !is_physical(interface)
    {
        anyhow::bail!("invalid or non-physical network interface: {interface}");
    }
    Ok(())
}

fn is_physical(interface: &str) -> bool {
    let base = PathBuf::from("/sys/class/net").join(interface);
    base.join("device").exists()
}

fn is_wireless_physical(interface: &str) -> bool {
    let base = PathBuf::from("/sys/class/net").join(interface);
    base.join("device").exists() && base.join("wireless").exists()
}

fn inspect_wired(interface: &str) -> (Option<bool>, Option<bool>) {
    let properties = Command::new("ethtool")
        .arg(interface)
        .stdin(Stdio::null())
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).into_owned());
    let supported = properties
        .as_deref()
        .and_then(|text| property(text, "Supports Wake-on:").map(|value| value.contains('g')));
    let enabled = properties
        .as_deref()
        .and_then(|text| property(text, "Wake-on:").map(|value| value.contains('g')));
    (supported, enabled)
}

fn inspect_wireless(interface: &str) -> (Option<bool>, Option<bool>) {
    let Some(phy) = wireless_phy(interface) else {
        return (None, None);
    };
    let supported = command_stdout("iw", &["phy", &phy, "info"])
        .map(|text| text.contains("wake up on magic packet"));
    let runtime = command_stdout("iw", &["phy", &phy, "wowlan", "show"])
        .map(|text| text.contains("wake up on magic packet"));
    let power = std::fs::read_to_string(
        Path::new("/sys/class/net")
            .join(interface)
            .join("device/power/wakeup"),
    )
    .ok()
    .map(|value| value.trim() == "enabled");
    let enabled = match (runtime, power) {
        (Some(runtime), Some(power)) => Some(runtime && power),
        (Some(runtime), None) => Some(runtime),
        (None, power) => power,
    };
    (supported, enabled)
}

fn configure_wireless_runtime(interface: &str, enabled: bool) -> Result<()> {
    let phy = wireless_phy(interface).context("cannot determine the Wi-Fi phy")?;
    let mut command = Command::new("iw");
    command.args(["phy", &phy, "wowlan"]);
    if enabled {
        command.args(["enable", "magic-packet"]);
    } else {
        command.arg("disable");
    }
    checked(
        command.stdin(Stdio::null()),
        "setting the Wi-Fi WoWLAN runtime mode",
    )?;
    let wakeup = Path::new("/sys/class/net")
        .join(interface)
        .join("device/power/wakeup");
    std::fs::write(&wakeup, if enabled { "enabled" } else { "disabled" }).with_context(|| {
        format!(
            "setting {} (run this command as root or through an authorized Polkit session)",
            wakeup.display()
        )
    })?;
    Ok(())
}

fn wireless_phy(interface: &str) -> Option<String> {
    std::fs::canonicalize(Path::new("/sys/class/net").join(interface).join("phy80211"))
        .ok()?
        .file_name()?
        .to_str()
        .map(ToOwned::to_owned)
}

fn command_stdout(program: &str, arguments: &[&str]) -> Option<String> {
    let output = Command::new(program)
        .args(arguments)
        .stdin(Stdio::null())
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).into_owned())
}

fn default_route_interface() -> Option<String> {
    let output = Command::new("ip")
        .args(["-o", "route", "show", "default"])
        .stdin(Stdio::null())
        .output()
        .ok()?;
    let text = String::from_utf8_lossy(&output.stdout);
    let fields = text.split_whitespace().collect::<Vec<_>>();
    let index = fields.iter().position(|field| *field == "dev")?;
    fields.get(index + 1).map(|value| value.to_string())
}

fn active_connection(interface: &str) -> Option<String> {
    let output = Command::new("nmcli")
        .args(["-g", "GENERAL.CONNECTION", "device", "show", interface])
        .stdin(Stdio::null())
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let value = String::from_utf8_lossy(&output.stdout).trim().to_string();
    (!value.is_empty() && value != "--").then_some(value)
}

fn property<'a>(text: &'a str, name: &str) -> Option<&'a str> {
    text.lines()
        .map(str::trim)
        .find_map(|line| line.strip_prefix(name).map(str::trim))
}

fn checked(command: &mut Command, context: &str) -> Result<()> {
    let output = command.output().with_context(|| context.to_string())?;
    if output.status.success() {
        return Ok(());
    }
    let detail = String::from_utf8_lossy(&output.stderr).trim().to_string();
    anyhow::bail!("{context}: {detail}")
}

fn tri_state(value: Option<bool>) -> String {
    match value {
        Some(true) => t!("wol.yes").to_string(),
        Some(false) => t!("wol.no").to_string(),
        None => t!("wol.unknown_ethtool").to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::property;

    #[test]
    fn parses_ethtool_wake_properties_without_confusing_them() {
        let sample = "Supports Wake-on: pumbg\nWake-on: g\n";
        assert_eq!(property(sample, "Supports Wake-on:"), Some("pumbg"));
        assert_eq!(property(sample, "Wake-on:"), Some("g"));
    }
}
