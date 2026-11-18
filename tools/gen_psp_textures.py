#!/usr/bin/env python3
"""Compile RGBA8 PNGs under assets/psp/textures into PSP GE assets.

Uses only Python's standard library. PNG inputs must be non-interlaced,
8-bit RGB or RGBA. All dimensions are checked against the manifest.
"""
from __future__ import annotations
import argparse
import struct
import zlib
from pathlib import Path

BUTTONS = [
    ("circle", "g_psp_button_circle", 16, 16),
    ("cross", "g_psp_button_cross", 16, 16),
    ("square", "g_psp_button_square", 16, 16),
    ("triangle", "g_psp_button_triangle", 16, 16),
    ("l", "g_psp_button_l", 32, 16),
    ("r", "g_psp_button_r", 32, 16),
    ("select", "g_psp_button_select", 32, 16),
    ("dpad_ud", "g_psp_button_dpad_ud", 16, 16),
    ("dpad_lr", "g_psp_button_dpad_lr", 16, 16),
    ("dpad_lrud", "g_psp_button_dpad_lrud", 16, 16),
]
HUD = [
    ("reserve_on", "g_hud_ico_reserve_on", 32, 16),
    ("reserve_off", "g_hud_ico_reserve_off", 32, 16),
    ("speed_selected", "g_hud_ico_speed_sel", 16, 16),
    ("speed_unselected", "g_hud_ico_speed_uns", 16, 16),
]
MESSAGE = [("font", 256, 256), ("grapple", 16, 16), ("missile", 32, 16),
           ("powerbomb", 16, 16), ("supermissile", 16, 16), ("xray", 16, 16)]


def decode_rgba_png(path: Path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, compressed, dims = 8, [], None
    while pos < len(data):
        if pos + 12 > len(data):
            raise ValueError(f"{path}: truncated PNG chunk")
        n = struct.unpack_from(">I", data, pos)[0]
        kind = data[pos + 4:pos + 8]
        start, end = pos + 8, pos + 8 + n
        if end + 4 > len(data):
            raise ValueError(f"{path}: truncated PNG data")
        payload = data[start:end]
        crc = struct.unpack_from(">I", data, end)[0]
        if zlib.crc32(kind + payload) & 0xffffffff != crc:
            raise ValueError(f"{path}: CRC error in {kind!r}")
        if kind == b"IHDR":
            w, h, depth, color, comp, filt, interlace = struct.unpack(">IIBBBBB", payload)
            if depth != 8 or color not in (2, 6) or comp or filt or interlace:
                raise ValueError(f"{path}: expected non-interlaced 8-bit RGB or RGBA PNG")
            dims = (w, h, color)
        elif kind == b"IDAT":
            compressed.append(payload)
        elif kind == b"IEND":
            break
        pos = end + 4
    if dims is None:
        raise ValueError(f"{path}: missing IHDR")
    w, h, color = dims
    raw = zlib.decompress(b"".join(compressed))
    bpp = 3 if color == 2 else 4
    stride = w * bpp
    if len(raw) != h * (stride + 1):
        raise ValueError(f"{path}: unexpected decompressed size")
    decoded = bytearray(h * stride)
    src = 0
    for y in range(h):
        filt = raw[src]
        src += 1
        row = bytearray(raw[src:src + stride])
        src += stride
        prev = decoded[(y - 1) * stride:y * stride] if y else bytearray(stride)
        for x in range(stride):
            a = row[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if filt == 1:
                row[x] = (row[x] + a) & 255
            elif filt == 2:
                row[x] = (row[x] + b) & 255
            elif filt == 3:
                row[x] = (row[x] + ((a + b) >> 1)) & 255
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                row[x] = (row[x] + predictor) & 255
            elif filt != 0:
                raise ValueError(f"{path}: unsupported PNG filter {filt}")
        decoded[y * stride:(y + 1) * stride] = row
    if color == 6:
        return w, h, bytes(decoded)
    rgba = bytearray(w * h * 4)
    for i in range(w * h):
        rgba[i * 4:i * 4 + 3] = decoded[i * 3:i * 3 + 3]
        rgba[i * 4 + 3] = 255
    return w, h, bytes(rgba)


def load(path: Path, width: int, height: int) -> bytes:
    w, h, rgba = decode_rgba_png(path)
    if (w, h) != (width, height):
        raise ValueError(f"{path}: expected {width}x{height}, got {w}x{h}")
    return rgba


def c_header(entries, source: Path, mutable: bool) -> str:
    out = ["#pragma once", "#include <stdint.h>",
           "/* Generated from assets/psp/textures; do not edit by hand. */", ""]
    for filename, symbol, w, h in entries:
        rgba = load(source / filename, w, h)
        words = []
        for i in range(0, len(rgba), 4):
            r, g, b, a = rgba[i:i + 4]
            words.append(r | (g << 8) | (b << 16) | (a << 24))
        qualifier = "" if mutable else "const "
        out.append(f"static {qualifier}uint32_t {symbol}[{w} * {h}] __attribute__((aligned(64))) = {{")
        for i in range(0, len(words), 8):
            out.append("  " + ", ".join(f"0x{x:08x}" for x in words[i:i + 8]) + ",")
        out.extend(["};", ""])
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, default=Path("assets/psp/textures"))
    ap.add_argument("--output", type=Path, default=Path("build/generated"))
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "src/psp").mkdir(parents=True, exist_ok=True)
    (args.output / "src/psp/psp_button_icons.h").write_text(
        c_header([(f"buttons/{f}.png", s, w, h) for f, s, w, h in BUTTONS], args.source, False), encoding="utf-8")
    (args.output / "src/psp/psp_hud_icons.h").write_text(
        c_header([(f"hud/{f}.png", s, w, h) for f, s, w, h in HUD], args.source, True), encoding="utf-8")
    msg_out = args.output / "assets/psp/message"
    msg_out.mkdir(parents=True, exist_ok=True)
    for filename, w, h in MESSAGE:
        (msg_out / f"{filename}.rgba").write_bytes(load(args.source / "message" / f"{filename}.png", w, h))

if __name__ == "__main__":
    main()
