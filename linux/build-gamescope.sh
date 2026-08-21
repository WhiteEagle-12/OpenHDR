#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cache_root=${XDG_CACHE_HOME:-$HOME/.cache}/openhdr
source_dir=$cache_root/gamescope-3.16.24
build_dir=$source_dir/build-openhdr
output_dir=$script_dir/bin

for command_name in git meson ninja glslang rg pkg-config wayland-scanner cc c++; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Missing build command: $command_name" >&2
    echo "CachyOS/Arch dependencies: sudo pacman -S --needed base-devel git meson ninja stb wayland-protocols vulkan-headers libliftoff" >&2
    exit 1
  }
done

if ! pkg-config --exists wayland-client x11 cairo pangocairo; then
  echo "Missing menu development libraries (wayland-client, X11, Cairo, or Pango)." >&2
  echo "CachyOS/Arch: sudo pacman -S --needed wayland libx11 cairo pango" >&2
  exit 1
fi

if [[ ! -d $source_dir/.git ]]; then
  mkdir -p "$cache_root"
  git clone --branch 3.16.24 --depth 1 --recurse-submodules \
    https://github.com/ValveSoftware/gamescope.git "$source_dir"
  git -C "$source_dir" apply "$script_dir/gamescope-openhdr.patch"
fi

if ! rg -q "const bool bApplyLook" "$source_dir/src/color_helpers.cpp" ||
   ! rg -q "cv_openhdr_enabled" "$source_dir/src/rendervulkan.cpp" ||
   ! rg -q "c_layerCount == 1" "$source_dir/src/shaders/composite.h"; then
  echo "Cached Gamescope source predates OpenHDR's signed-scRGB patch:" >&2
  echo "  $source_dir" >&2
  echo "Move that cache directory aside, then run this build again." >&2
  exit 1
fi

if ! rg -q "return perform_3dlut_native" "$source_dir/src/shaders/colorimetry.h" ||
   ! rg -q 'lut3DDescriptor\[i\]\.sampler = m_device->sampler\(linearState\)' "$source_dir/src/rendervulkan.cpp"; then
  if git -C "$source_dir" apply --check \
      --include=src/shaders/colorimetry.h \
      --include=src/rendervulkan.cpp \
      "$script_dir/gamescope-openhdr.patch"; then
    git -C "$source_dir" apply \
      --include=src/shaders/colorimetry.h \
      --include=src/rendervulkan.cpp \
      "$script_dir/gamescope-openhdr.patch"
  else
    echo "Cached Gamescope source cannot be upgraded to OpenHDR's native-trilinear LUT path:" >&2
    echo "  $source_dir" >&2
    echo "Move that cache directory aside, then run this build again." >&2
    exit 1
  fi
fi

if [[ ! -f $build_dir/build.ninja ]]; then
  meson setup "$build_dir" "$source_dir" \
    --prefix=/usr \
    --buildtype=release \
    -Denable_gamescope_wsi_layer=false \
    -Denable_openvr_support=false \
    -Denable_tests=false \
    -Dpipewire=disabled \
    -Davif_screenshots=disabled \
    -Ddrm_backend=enabled
fi

ninja -C "$build_dir" src/gamescope
mkdir -p "$output_dir"
install -m 0755 "$build_dir/src/gamescope" "$output_dir/gamescope-openhdr"
menu_build_dir=$build_dir/openhdr-menu
mkdir -p "$menu_build_dir"
wayland-scanner client-header \
  "$source_dir/protocol/gamescope-action-binding.xml" \
  "$menu_build_dir/gamescope-action-binding-client-protocol.h"
wayland-scanner private-code \
  "$source_dir/protocol/gamescope-action-binding.xml" \
  "$menu_build_dir/gamescope-action-binding-protocol.c"
cc -O2 -c "$menu_build_dir/gamescope-action-binding-protocol.c" \
  -o "$menu_build_dir/gamescope-action-binding-protocol.o" \
  $(pkg-config --cflags wayland-client)
c++ -std=c++20 -O2 -pthread \
  -I"$menu_build_dir" \
  "$script_dir/menu/openhdr-menu.cpp" \
  "$menu_build_dir/gamescope-action-binding-protocol.o" \
  -o "$output_dir/openhdr-menu" \
  $(pkg-config --cflags --libs wayland-client x11 cairo pangocairo)
echo "Built $output_dir/gamescope-openhdr"
echo "Built $output_dir/openhdr-menu"
