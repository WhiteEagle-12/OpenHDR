# OpenHDR

A Windows desktop overlay that expands SDR games to scRGB HDR. It captures the
focused game window, runs a 49³ LUT plus RTX-style peak / paper-white / contrast
/ saturation / deband controls, and draws the result on a click-through HDR
window. Nothing is injected into the game.

## Requirements

- Windows 10 1903+ (Windows 11 preferred; it can hide the capture border)
- An HDR display with Windows HDR turned on
- Visual Studio 2022 C++ toolset (Build Tools is enough)
- A 64-bit game running **borderless** or **windowed** (exclusive fullscreen often cannot be captured)

## Build

```bat
msbuild OpenHDR.sln /p:Configuration=Release /p:Platform=x64
```

Output: `bin\Release\OpenHDR.exe` plus `lut3d_49.bin` and `rtx_control_atlas.bin`.

## Use

1. Run `OpenHDR.exe`.
2. Focus a game.
3. Press **Alt+X** for settings.

Settings are stored in `%LOCALAPPDATA%\OpenHDR\settings.ini`.

The overlay stays click-through while the menu is closed, so it does not steal
input. Use borderless / windowed mode. Exclusive fullscreen, DRM-protected
frames, and some anti-cheat capture blocks will show black or no overlay.

Latency is about one to two display frames (capture + compose). The LUT itself
is a fraction of a millisecond.

## License

MIT
