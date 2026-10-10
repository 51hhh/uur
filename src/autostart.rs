//! Optional graphical-session autostart.
//!
//! A systemd user unit is preferred because it participates in the desktop
//! session lifetime and reports startup failures. Desktops without a systemd
//! user manager retain the portable XDG autostart fallback.

use anyhow::{Context, Result};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const UNIT_NAME: &str = "uur.service";

pub fn enable() -> Result<()> {
    let executable = std::env::current_exe().context("resolving uur executable")?;
    if systemd_user_available() {
        enable_systemd(&executable)?;
        let desktop = desktop_path()?;
        if desktop.exists() {
            std::fs::remove_file(desktop)?;
        }
        println!(
            "{}",
            t!(
                "autostart.enabled",
                path = unit_path()?.display().to_string()
            )
        );
    } else {
        enable_xdg(&executable)?;
        println!(
            "{}",
            t!(
                "autostart.enabled",
                path = desktop_path()?.display().to_string()
            )
        );
    }
    Ok(())
}

pub fn disable() -> Result<()> {
    let unit = unit_path()?;
    if unit.exists() {
        if systemd_user_available() {
            let _ = systemctl(&["disable", "--now", UNIT_NAME]);
        }
        std::fs::remove_file(&unit)?;
        if systemd_user_available() {
            systemctl(&["daemon-reload"])?;
        }
    }
    let desktop = desktop_path()?;
    if desktop.exists() {
        std::fs::remove_file(desktop)?;
    }
    println!("{}", t!("autostart.disabled"));
    Ok(())
}

pub fn status() -> Result<()> {
    let unit = unit_path()?;
    let desktop = desktop_path()?;
    let enabled = if unit.is_file() {
        Some(unit)
    } else if desktop.is_file() {
        Some(desktop)
    } else {
        None
    };
    println!(
        "{}",
        match enabled {
            Some(path) => t!(
                "autostart.status_enabled",
                path = path.display().to_string()
            ),
            None => t!("autostart.status_disabled"),
        }
    );
    Ok(())
}

fn enable_systemd(executable: &Path) -> Result<()> {
    let path = unit_path()?;
    std::fs::create_dir_all(path.parent().context("systemd user path has no parent")?)?;
    std::fs::write(&path, systemd_unit(executable))?;
    systemctl(&["daemon-reload"])?;
    systemctl(&["enable", "--now", UNIT_NAME])
}

fn enable_xdg(executable: &Path) -> Result<()> {
    let path = desktop_path()?;
    std::fs::create_dir_all(path.parent().context("autostart path has no parent")?)?;
    let exec = desktop_exec_argument(&executable.to_string_lossy());
    std::fs::write(
        path,
        format!(
            "[Desktop Entry]\nType=Application\nName=UU Remote\nComment=Start the managed UU Remote session\nExec={exec} run\nIcon=uur\nTerminal=false\nX-GNOME-Autostart-enabled=true\n"
        ),
    )?;
    Ok(())
}

fn systemd_unit(executable: &Path) -> String {
    let executable = systemd_exec_argument(&executable.to_string_lossy());
    format!(
        "[Unit]\n\
         Description=UU Remote managed desktop session\n\
         PartOf=graphical-session.target\n\
         After=graphical-session.target xdg-desktop-portal.service pipewire.service\n\
         \n\
         [Service]\n\
         Type=simple\n\
         ExecStart={executable} run\n\
         Restart=on-failure\n\
         RestartSec=5s\n\
         TimeoutStopSec=30s\n\
         \n\
         [Install]\n\
         WantedBy=graphical-session.target\n"
    )
}

fn systemctl(arguments: &[&str]) -> Result<()> {
    let status = Command::new("systemctl")
        .arg("--user")
        .args(arguments)
        .status()
        .context("running systemctl --user")?;
    if !status.success() {
        anyhow::bail!("systemctl --user {} failed ({status})", arguments.join(" "));
    }
    Ok(())
}

fn systemd_user_available() -> bool {
    Command::new("systemctl")
        .args(["--user", "show-environment"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|status| status.success())
}

fn config_home() -> Result<PathBuf> {
    std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
        .context("cannot determine XDG configuration directory")
}

fn desktop_path() -> Result<PathBuf> {
    Ok(config_home()?.join("autostart/uur.desktop"))
}

fn unit_path() -> Result<PathBuf> {
    Ok(config_home()?.join("systemd/user").join(UNIT_NAME))
}

fn desktop_exec_argument(value: &str) -> String {
    if value.bytes().all(|byte| {
        byte.is_ascii_alphanumeric() || matches!(byte, b'/' | b'_' | b'-' | b'.' | b':')
    }) {
        return value.to_string();
    }
    format!("\"{}\"", value.replace('\\', "\\\\").replace('"', "\\\""))
}

fn systemd_exec_argument(value: &str) -> String {
    let escaped = value
        .replace('%', "%%")
        .replace('\\', "\\\\")
        .replace('"', "\\\"");
    format!("\"{escaped}\"")
}

#[cfg(test)]
mod tests {
    use super::{desktop_exec_argument, systemd_unit};
    use std::path::Path;

    #[test]
    fn desktop_exec_paths_are_quoted_only_when_needed() {
        assert_eq!(desktop_exec_argument("/usr/bin/uur"), "/usr/bin/uur");
        assert_eq!(
            desktop_exec_argument("/opt/UU Remote/uur"),
            "\"/opt/UU Remote/uur\""
        );
    }

    #[test]
    fn systemd_unit_escapes_exec_paths_and_tracks_graphical_session() {
        let unit = systemd_unit(Path::new("/opt/UU % Remote/uur"));
        assert!(unit.contains("PartOf=graphical-session.target"));
        assert!(unit.contains("WantedBy=graphical-session.target"));
        assert!(unit.contains("ExecStart=\"/opt/UU %% Remote/uur\" run"));
    }
}
