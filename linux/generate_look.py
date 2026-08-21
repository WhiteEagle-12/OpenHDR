#!/usr/bin/env python3
"""Convert OpenHDR's binary LUT and control atlas to a Gamescope .cube look."""

import argparse
import array
import math
from pathlib import Path

import numpy as np

N = 49
ATLAS_KNOTS = 17 * 10 * 9 * 64


def load_floats(path: Path) -> array.array:
    values = array.array("f")
    with path.open("rb") as stream:
        values.fromfile(stream, path.stat().st_size // values.itemsize)
    if values.itemsize != 4:
        raise RuntimeError("OpenHDR assets require 32-bit floats")
    return values


def axis(value: float, origin: float, step: float, count: int) -> tuple[int, float]:
    value = min(max((value - origin) / step, 0.0), count - 1.0)
    index = min(math.floor(value), count - 2)
    return index, value - index


def tone(atlas: array.array, luma: float, peak: float, gray: float, contrast: float) -> float:
    ip, fp = axis(peak, 400.0, 100.0, 17)
    ig, fg = axis(gray, 10.0, 10.0, 10)
    ic, fc = axis(contrast, 0.0, 25.0, 9)
    pos = min(max(luma, 0.0), 1.0) * 63.0
    knot = min(math.floor(pos), 62)
    fk = pos - knot

    def curve(p: int, g: int, c: int) -> float:
        offset = (((p * 10 + g) * 9 + c) * 64) + knot
        return atlas[offset] + (atlas[offset + 1] - atlas[offset]) * fk

    c000, c100 = curve(ip, ig, ic), curve(ip + 1, ig, ic)
    c010, c110 = curve(ip, ig + 1, ic), curve(ip + 1, ig + 1, ic)
    c001, c101 = curve(ip, ig, ic + 1), curve(ip + 1, ig, ic + 1)
    c011, c111 = curve(ip, ig + 1, ic + 1), curve(ip + 1, ig + 1, ic + 1)
    x00 = c000 + (c100 - c000) * fp
    x10 = c010 + (c110 - c010) * fp
    x01 = c001 + (c101 - c001) * fp
    x11 = c011 + (c111 - c011) * fp
    y0 = x00 + (x10 - x00) * fg
    y1 = x01 + (x11 - x01) * fg
    return y0 + (y1 - y0) * fc


def saturation_scale(atlas: array.array, saturation: float) -> float:
    scales = atlas[ATLAS_KNOTS : ATLAS_KNOTS + 9]
    position = min(max(saturation / 25.0, 0.0), 8.0)
    index = int(position)
    if index >= 8:
        return scales[8]
    return scales[index] + (scales[index + 1] - scales[index]) * (position - index)


def encode_g22(value: float) -> float:
    # Gamescope applies a Gamma-2.2 source EOTF after the look. OpenHDR's LUT
    # stores linear scRGB, where 1.0 is 80 nits, so encode it for that stage.
    # Normalize scRGB's 80-nit units into a 2,000-nit Gamescope SDR container.
    # The 2,000-nit headroom covers the atlas's complete peak-control range.
    normalized = value * 80.0 / 2000.0
    # OpenHDR's scRGB LUT is signed. Preserve that sign through the .cube
    # transport; the paired Gamescope patch applies the matching signed
    # Gamma-2.2 decode before the normal linear HDR color conversion.
    return math.copysign(abs(normalized) ** (1.0 / 2.2), normalized)


def tone_array(atlas: np.ndarray, luma: np.ndarray, peak: float, gray: float, contrast: float) -> np.ndarray:
    ip, fp = axis(peak, 400.0, 100.0, 17)
    ig, fg = axis(gray, 10.0, 10.0, 10)
    ic, fc = axis(contrast, 0.0, 25.0, 9)
    position = np.clip(luma, 0.0, 1.0) * 63.0
    knot = np.minimum(np.floor(position).astype(np.intp), 62)
    fk = position - knot
    curves = atlas[:ATLAS_KNOTS].reshape(17, 10, 9, 64)

    def curve(p: int, g: int, c: int) -> np.ndarray:
        values = curves[p, g, c]
        return values[knot] + (values[knot + 1] - values[knot]) * fk

    x00 = curve(ip, ig, ic) + (curve(ip + 1, ig, ic) - curve(ip, ig, ic)) * fp
    x10 = curve(ip, ig + 1, ic) + (curve(ip + 1, ig + 1, ic) - curve(ip, ig + 1, ic)) * fp
    x01 = curve(ip, ig, ic + 1) + (curve(ip + 1, ig, ic + 1) - curve(ip, ig, ic + 1)) * fp
    x11 = curve(ip, ig + 1, ic + 1) + (curve(ip + 1, ig + 1, ic + 1) - curve(ip, ig + 1, ic + 1)) * fp
    y0 = x00 + (x10 - x00) * fg
    y1 = x01 + (x11 - x01) * fg
    return y0 + (y1 - y0) * fc


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lut", required=True, type=Path)
    parser.add_argument("--atlas", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--peak", type=float, default=1000.0)
    parser.add_argument("--paper-white", type=float, default=65.0)
    parser.add_argument("--contrast", type=float, default=120.0)
    parser.add_argument("--saturation", type=float, default=110.0)
    parser.add_argument("--output-scale", type=float, default=100.0)
    parser.add_argument("--black-floor", type=float, default=20.0)
    args = parser.parse_args()

    lut = np.fromfile(args.lut, dtype=np.float32)
    atlas = np.fromfile(args.atlas, dtype=np.float32)
    if lut.size != N * N * N * 3:
        raise RuntimeError(f"unexpected LUT size: {lut.size} floats")
    if atlas.size != ATLAS_KNOTS + 9:
        raise RuntimeError(f"unexpected atlas size: {atlas.size} floats")

    sat = saturation_scale(atlas, args.saturation)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="ascii") as output:
        output.write('TITLE "OpenHDR 49^3 LUT + RTX control atlas"\n')
        output.write(f"LUT_3D_SIZE {N}\n")
        # Build in Windows' r,g,b storage order, then transpose to .cube's
        # red-fast b,g,r row order. Vectorization cuts live menu rebuilds from
        # roughly 1.65 seconds to a small fraction of a second.
        axis_values = np.linspace(0.0, 1.0, N, dtype=np.float64)
        r, g, b = np.meshgrid(axis_values, axis_values, axis_values, indexing="ij")
        luma = r * 0.2126 + g * 0.7152 + b * 0.0722
        hdr = lut.reshape(N, N, N, 3).astype(np.float64)
        new_y = np.maximum(tone_array(atlas, luma, args.peak, args.paper_white, args.contrast), 1e-6)
        default_y = np.maximum(tone_array(atlas, luma, 1000.0, 50.0, 100.0), 1e-6)
        hdr *= (new_y / default_y)[..., None]
        hdr_y = np.maximum(np.sum(hdr * np.array((0.2126, 0.7152, 0.0722)), axis=-1), 1e-6)
        hdr = hdr_y[..., None] + (hdr - hdr_y[..., None]) * sat
        black_floor = max(args.black_floor / 1000.0, 1e-6)
        gate = np.clip(luma / black_floor, 0.0, 1.0)
        gate = gate * gate * (3.0 - 2.0 * gate)
        normalized = hdr * gate[..., None] * max(args.output_scale, 0.0) / 100.0 * 80.0 / 2000.0
        encoded = np.sign(normalized) * np.power(np.abs(normalized), 1.0 / 2.2)
        rows = encoded.transpose(2, 1, 0, 3).reshape(-1, 3)
        np.savetxt(output, rows, fmt="%.9g")



if __name__ == "__main__":
    main()
