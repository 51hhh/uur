# UU Windows feature map

The current UU server advertises these capability families. The Linux route is
organized by host subsystem so each backend can evolve independently.

| UU capability family | Linux integration |
| --- | --- |
| Desktop, window, and emulator capture | X11/Wine or portal/PipeWire frame source |
| Monitor, second screen, virtual display | XRandR topology or portal monitor/window/virtual sources |
| Resolution, orientation, HiDPI, zoom | Display topology and stream geometry adapters |
| 90/144 fps, true color, chroma, HDR | Frame format and encoder capability negotiation |
| Keyboard, mouse, smart/virtual mouse | RemoteDesktop portal, uinput, XTest |
| Touch, pen, gamepad | uinput tablet and future multitouch/gamepad devices |
| Speaker, microphone, remote mute | Wine WASAPI over PulseAudio, pipewire-pulse, or ALSA |
| Clipboard and phone text input | Portal clipboard and X11 selections |
| File transfer v1-v4 | Wine-visible paths now; native XDG staging next |
| Terminal and multiple sessions | Native PTY with ConPTY and mux ABI adapters |
| Port mapping | Native host socket broker |
| Quick Launch and remote application list | XDG desktop catalog and authenticated launcher |
| Wallpaper and privacy wallpaper | Desktop wallpaper providers and capture routing |
| Privacy screen | Compositor output policy or a dedicated virtual output |
| Wake-on-LAN | Physical NIC configuration and UU capability reporting |
| Lock, reboot, shutdown, task manager | logind/Polkit and desktop-native actions |
| Auto unlock and login-screen access | Opt-in GNOME screen-shield dismissal for logged-in sessions; display-manager integration for pre-login access |
| Controlled update | Official release feed and compatibility probes |

The [App Store release history](https://apps.apple.com/hk/app/uu%E8%BF%9C%E7%A8%8B-%E8%BF%9C%E7%A8%8B%E5%8A%9E%E5%85%AC-%E6%B8%B8%E6%88%8F%E4%B8%B2%E6%B5%81/id1642306791?platform=mac)
also confirms multi-session terminal support, view-only mode, automatic unlock,
and remote terminal access on current Apple clients. The server capability
table is broader and is the implementation map used by uur.
