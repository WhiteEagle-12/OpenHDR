# OpenHDR

A Windows desktop overlay that expands SDR games to scRGB HDR. It captures the
focused game window, runs one fused compute kernel (49³ LUT + deband + peak /
paper-white / contrast / saturation) straight into an scRGB swapchain, and
presents. Nothing is injected into the game.

The original capture overlay remains Windows-only. On Linux, `openhdr-linux`
runs the game inside a dedicated Gamescope build, converts the original
`lut3d_49.bin` into a 49³ Gamescope look, and bakes the original RTX control
atlas into that look. It is the OpenHDR transform, not Gamescope's generic
inverse tone mapper.

With the menu closed that present path is one dispatch and a tearing Present —
about 0.03 ms at 1080p and under a third of a millisecond at 4K on a 5070.
Window capture still adds about one display frame of latency.

## Requirements

- Windows 10 1903+ (Windows 11 preferred; it can hide the capture border)
- An HDR display with Windows HDR turned on
- Visual Studio 2022 C++ toolset (Build Tools is enough)
- A 64-bit game running **borderless** or **windowed** (exclusive fullscreen often cannot be captured)

### Linux

- A Wayland desktop with HDR enabled (KDE Plasma 6 is recommended)
- An HDR display
- [Gamescope](https://github.com/ValveSoftware/gamescope)

On CachyOS or Arch Linux, install the runtime and build dependencies:

```sh
sudo pacman -S --needed gamescope base-devel git meson ninja stb wayland-protocols vulkan-headers libliftoff libx11 cairo pango
```

## Build

```bat
msbuild OpenHDR.sln /p:Configuration=Release /p:Platform=x64
```

Output: `bin\Release\OpenHDR.exe` plus `lut3d_49.bin` and `rtx_control_atlas.bin`.

### Linux

Build the dedicated compositor once:

```sh
./linux/build-gamescope.sh
```

This checks out Gamescope 3.16.24 in `~/.cache/openhdr`, applies
`linux/gamescope-openhdr.patch`, and writes the local compositor and menu to
`linux/bin/gamescope-openhdr` and `linux/bin/openhdr-menu`. It does not replace
the system Gamescope binary.

Run a game while explicitly selecting the current Linux defaults:

```sh
./openhdr-linux --peak 1000 --paper-white 65 --contrast 120 --saturation 110 -- /path/to/game
```

For a Steam game, set its launch options to:

```text
/absolute/path/to/OpenHDR/openhdr-linux -- %command%
```

On a multi-GPU system, select the GPU that drives the desktop. For example,
the RTX 5070 (PCI ID `10de:2f04`) can be selected with:

```text
/absolute/path/to/OpenHDR/openhdr-linux --gpu 10de:2f04 -- %command%
```

If exactly one GPU has connected displays, `openhdr-linux` now detects and
selects it automatically. It also applies matching DXVK and VKD3D device-name
filters so a Proton game does not render on a different GPU from Gamescope.
`--gpu` or `OPENHDR_GPU` remains available as an explicit override.

The launcher also advertises the primary display's active resolution and
refresh rate to both Gamescope and the game. This avoids Gamescope's 1280x720
nested-mode default. Override detection when needed with, for example,
`--width 3840 --height 2160 --refresh 240` (or the corresponding
`OPENHDR_WIDTH`, `OPENHDR_HEIGHT`, and `OPENHDR_REFRESH` environment values).

Use `./openhdr-linux --help` for all options. Generated looks are cached by
control values under `~/.cache/openhdr`.

Press **Alt+Z** in game to open or close the compositor-native OpenHDR menu.
Press **Alt+X** to toggle OpenHDR processing directly without opening the menu.
An Xwayland passive grab is the single shortcut owner; the matching Gamescope
bindings remain registered but disarmed so one physical key press cannot arrive
through both paths and toggle twice. The menu exposes
peak brightness, paper white, contrast, saturation, output intensity, deband,
dither, and shadow controls. The top-right switch completely removes the look
and disables preprocessing; it is a true pass-through, not a neutral preset.
Changes are persisted per game under `~/.config/openhdr/games/` and become that
game's next-launch defaults. Steam profiles use the app ID (for example,
`steam-264710.conf` for Subnautica); direct launches use the executable name.
Set `OPENHDR_PROFILE` to override the profile key. The initial defaults are
1000 nits, 65 paper white, 120% contrast, and 110% saturation.

The menu window is not mapped while closed, so it cannot intercept game input
or add a transparent compositor layer. Before unmapping, it presents a clear
frame and forces an opacity repaint so Gamescope cannot retain the previous
overlay frame. While visible it requests Gamescope overlay input focus; closing
it returns focus to the game. While the menu is visible, the signed 49^3 look
and tone controls remain live. Deband/dither pause until the menu closes because
NVIDIA's driver rejects Gamescope's expanded multi-layer pre-pass shader; normal
one-layer gameplay retains the full preprocessing path.

The Linux build changes Gamescope's internal compositor LUT from 17³ to 49³,
uses hardware trilinear sampling for both the OpenHDR look and the final
display LUT to match the Windows kernel, and integrates the same deband and
TPDF-dither operations directly into the compositor shader. Debanding remains
in the SDR source domain, while dithering is applied after output encoding as
one 10-bit PQ code step. This prevents the nonlinear HDR transform from
amplifying an input-domain 1/255 perturbation into visible multi-nit noise. The LUT
generator reproduces peak/paper-white/contrast interpolation
from `rtx_control_atlas.bin`, its saturation curve, and the black gate. Signed
scRGB components are carried through the look with a signed Gamma-2.2 bridge,
restored before Gamescope's linear Rec.709-to-HDR output conversion, and only
clipped if they remain outside the physical output gamut. The live menu updates
shader controls immediately and rebuilds the 49^3 look asynchronously when a
tone or color control changes.

## Use

1. Run `OpenHDR.exe`. A small settings window appears.
2. Click the game so it is focused.
3. Press **Alt+X** to attach. Press it again for settings.

OpenHDR only attaches to the focused game (same idea as Lossless Scaling's
scale hotkey). Browsers, Discord, Cursor, Explorer, and other desktop apps
are ignored. The process is per-monitor DPI aware and the overlay is sized
to the window's visible frame so the image is not stretched.

If another app already owns Alt+X, OpenHDR reports that at startup.

Settings are stored in `%LOCALAPPDATA%\OpenHDR\settings.ini`.

The overlay stays click-through while the menu is closed. Use borderless or
windowed mode. Exclusive fullscreen, DRM-protected frames, and some
anti-cheat capture blocks will show black or no overlay.

Capture latency is about one to two display frames. The fused kernel on the
present path is under 0.3 ms.

## License

MIT
