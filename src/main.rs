#[macro_use]
extern crate rust_i18n;

mod audio;
mod autostart;
mod bridge;
mod capture;
mod config;
mod doctor;
mod i18n;
mod inhibit;
mod input;
mod launcher;
mod protocol;
mod proxy;
mod session;
mod terminal;
mod upstream;
mod video;
mod wallpaper;
mod wine;
mod wol;

use anyhow::Result;
use clap::{Parser, Subcommand, ValueEnum};

// Locale catalogs are embedded from `locales/` at the crate root.
i18n!("locales", fallback = "en");

/// Native Linux companion for NetEase UU Remote.
///
/// uur runs the official Windows client inside a managed Wine prefix and
/// bridges its input and screen capture onto the real Linux desktop through
/// X11 XTest or the Wayland RemoteDesktop portal.
#[derive(Parser)]
#[command(name = "uur", version, about, arg_required_else_help = true)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// One-time provisioning: EULA gate, Wine prefix, official client download.
    Setup {
        /// Accept the NetEase UU Remote EULA non-interactively.
        #[arg(long)]
        accept_eula: bool,
    },
    /// Print the live environment report (sessions, wine, portals, backends).
    Doctor,
    /// Print the current X11 pointer position (verification helper).
    Pointer,
    /// Capture the desktop through the ScreenCast portal (video path).
    Capture,
    /// Start a full session: capture + bridge + the managed client.
    Run,
    /// Stop every uur-owned process and the managed Wine prefix.
    Stop,
    /// Manage optional XDG login autostart.
    Autostart {
        #[command(subcommand)]
        action: AutostartAction,
    },
    /// Inspect or configure wired Wake-on-LAN or Wi-Fi WoWLAN.
    Wol {
        #[command(subcommand)]
        action: WolAction,
    },
    /// Manage opt-in GNOME unlock after authenticated remote input.
    RemoteUnlock {
        #[command(subcommand)]
        action: RemoteUnlockAction,
    },
    /// Select the portal capture source used on Wayland.
    Display {
        #[command(subcommand)]
        action: DisplayAction,
    },
    /// Internal input bridge process. Not part of the public CLI.
    #[command(hide = true, name = "__bridge")]
    InternalBridge,
    /// Internal capture process. Not part of the public CLI.
    #[command(hide = true, name = "__capture")]
    InternalCapture,
    /// Internal native PTY bridge. Not part of the public CLI.
    #[command(hide = true, name = "__terminal")]
    InternalTerminal,
    /// Internal Linux wallpaper synchronizer. Not part of the public CLI.
    #[command(hide = true, name = "__wallpaper")]
    InternalWallpaper,
    /// Internal desktop suspend/idle inhibitor. Not part of the public CLI.
    #[command(hide = true, name = "__inhibit")]
    InternalInhibit,
    /// Internal foreground session supervisor. Not part of the public CLI.
    #[command(hide = true, name = "__run")]
    InternalRun,
    /// Internal authenticated XDG application launcher.
    #[command(hide = true, name = "__launcher")]
    InternalLauncher,
    /// Query NetEase's official release feed.
    Upstream {
        #[command(subcommand)]
        action: UpstreamAction,
    },
}

#[derive(Subcommand)]
enum UpstreamAction {
    /// Print the latest official UU Remote release known to NetEase.
    Check,
}

#[derive(Subcommand)]
enum AutostartAction {
    /// Start the managed session after graphical login.
    Enable,
    /// Remove the login autostart entry.
    Disable,
    /// Report whether login autostart is enabled.
    Status,
}

#[derive(Subcommand)]
enum WolAction {
    /// Report the selected physical adapter and Magic Packet state.
    Status {
        #[arg(long)]
        interface: Option<String>,
    },
    /// Enable Magic Packet wake for the adapter and its NetworkManager profile.
    Enable {
        #[arg(long)]
        interface: Option<String>,
    },
    /// Disable Magic Packet wake for the adapter and its NetworkManager profile.
    Disable {
        #[arg(long)]
        interface: Option<String>,
    },
}

#[derive(Subcommand)]
enum RemoteUnlockAction {
    /// Allow authenticated remote input to dismiss the GNOME lock screen.
    Enable,
    /// Keep the GNOME lock screen and its local authentication prompt.
    Disable,
    /// Report whether authenticated remote input may dismiss the lock screen.
    Status,
}

#[derive(Subcommand)]
enum DisplayAction {
    /// Show the configured source type.
    Status,
    /// Use one or more monitors, one window, or a portal-provided virtual display.
    Source { source: DisplaySource },
}

#[derive(Clone, ValueEnum)]
enum DisplaySource {
    Monitor,
    Window,
    Virtual,
}

impl DisplaySource {
    fn as_str(&self) -> &'static str {
        match self {
            Self::Monitor => "monitor",
            Self::Window => "window",
            Self::Virtual => "virtual",
        }
    }
}

fn main() -> Result<()> {
    // CLI output is routinely piped; a closed pipe is not an error.
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_DFL);
    }
    i18n::init();
    let cli = Cli::parse();
    match cli.command {
        Command::Setup { accept_eula } => wine::setup(accept_eula),
        Command::Doctor => doctor::report(),
        Command::Pointer => bridge::print_pointer(),
        Command::Capture => capture::run(),
        Command::Run => session::launch(),
        Command::Stop => session::stop(),
        Command::Autostart { action } => match action {
            AutostartAction::Enable => autostart::enable(),
            AutostartAction::Disable => autostart::disable(),
            AutostartAction::Status => autostart::status(),
        },
        Command::Wol { action } => match action {
            WolAction::Status { interface } => wol::status(interface.as_deref()),
            WolAction::Enable { interface } => wol::configure(interface.as_deref(), true),
            WolAction::Disable { interface } => wol::configure(interface.as_deref(), false),
        },
        Command::RemoteUnlock { action } => {
            let mut config = config::Config::load()?;
            match action {
                RemoteUnlockAction::Enable => {
                    config.unlock_on_remote_input = true;
                    config.store()?;
                    println!("{}", t!("remote_unlock.enabled"));
                    Ok(())
                }
                RemoteUnlockAction::Disable => {
                    config.unlock_on_remote_input = false;
                    config.store()?;
                    println!("{}", t!("remote_unlock.disabled"));
                    Ok(())
                }
                RemoteUnlockAction::Status => {
                    let key = if config.unlock_on_remote_input {
                        "remote_unlock.status_enabled"
                    } else {
                        "remote_unlock.status_disabled"
                    };
                    println!("{}", t!(key));
                    Ok(())
                }
            }
        }
        Command::Display { action } => {
            let mut config = config::Config::load()?;
            match action {
                DisplayAction::Status => {
                    println!("{}", t!("display.status", source = config.capture_source));
                    Ok(())
                }
                DisplayAction::Source { source } => {
                    config.capture_source = source.as_str().to_string();
                    config.capture_restore_token = None;
                    config.remote_desktop_restore_token = None;
                    config.store()?;
                    println!("{}", t!("display.changed", source = source.as_str()));
                    Ok(())
                }
            }
        }
        Command::InternalBridge => bridge::serve(),
        Command::InternalCapture => capture::run(),
        Command::InternalTerminal => terminal::serve(),
        Command::InternalWallpaper => wallpaper::serve(),
        Command::InternalInhibit => inhibit::serve(),
        Command::InternalRun => session::start(),
        Command::InternalLauncher => launcher::serve(),
        Command::Upstream {
            action: UpstreamAction::Check,
        } => upstream::check(),
    }
}
