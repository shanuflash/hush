# Hush

Mute your mic system-wide with a mouse button. Works in Discord, games, the browser, anything that uses your microphone.

Tiny Windows tray app. One exe, no installer, Windows 10/11 64-bit.

## Usage

Grab `Hush.exe` from [Releases](../../releases) and put it somewhere permanent, like `C:\Tools\Hush\`.

Run it. A mic icon shows up in the tray, green when live and red when muted.

Press the back side button on your mouse to mute or unmute. Left-clicking the tray icon toggles too.

If Windows shows "Windows protected your PC", click **More info** and then **Run anyway**. That happens with any exe that isn't code-signed.

## Settings

Right-click the tray icon.

- **Change hotkey**: press any mouse button except left/right, or any key. Esc cancels.
- **Mode**: toggle, push to talk, or push to mute
- **Indicator**: when the on-screen badge shows (off, flash on change, only while muted, only while live, or always), its style (Capsule, Orb, Edge, Ghost or Glow), and where it sits (top center by default)
- **Devices**: all microphones, or just the default one
- **Play sounds**, **Start with Windows**

## Notes

- If a game runs as administrator, Windows won't pass its input to Hush while it has focus, so the hotkey does nothing in that game. Run Hush as administrator too (right-click → Run as administrator), or don't run the game elevated.
- Exiting Hush unmutes your mic if Hush was the one that muted it, so it's never left dead with no tray icon to explain why.
- If you used a Discord mute keybind on the same button, remove it. Otherwise both fire and you end up double muted.
- Discord won't show its red mute icon since the mute happens at the Windows level. People just hear nothing.
- The on-screen badge doesn't show up in exclusive fullscreen. Borderless and windowed are fine.
- The badge is hidden from screenshots, OBS and Discord screen share.
- If you play competitive games in borderless mode, use "flash on change". A badge that stays on screen can cost a frame of latency on some setups.

## How it works

Mouse buttons are read with Raw Input in sink mode, the same passive way games read input. No hooks, no DLL injection, no driver, and it never touches other processes, so it shouldn't bother anti-cheat.

Muting goes through Core Audio (`IAudioEndpointVolume::SetMute`) on your capture devices, so it applies to every app at once.

The badge is a click-through layered window. It's drawn once when your mic state changes and Windows composites it from there, so it costs basically nothing while you play.

## Building

Needs Visual Studio Build Tools with the C++ workload. Then:

```
build.bat
```

## Uninstall

Right-click the tray icon, uncheck **Start with Windows**, then **Exit**. Delete the exe. Settings are stored under `HKEY_CURRENT_USER\Software\Hush` if you want to remove those too.
