# OpenHDR

A Windows desktop overlay that expands SDR games to scRGB HDR. It captures the
focused game window, runs one fused compute kernel (49³ LUT + deband + peak /
paper-white / contrast / saturation) straight into an scRGB swapchain, and
presents. Nothing is injected into the game.

With the menu closed that present path is one dispatch and a tearing Present —
about 0.03 ms at 1080p and under a third of a millisecond at 4K on a 5070.
Window capture still adds about one display frame of latency.

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

Capture latency is about one to two display frames. The fused kernel on the
present path is under 0.3 ms.

## License

MIT
