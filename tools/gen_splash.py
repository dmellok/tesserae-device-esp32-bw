#!/usr/bin/env python3
"""Generate a panel-native .bin splash for a Tesserae e-paper client.

Reads a square PNG logo (alpha composited onto white), Floyd-Steinberg
dithers a --width x --height canvas to the chosen palette, and packs to
the panel's native bit layout, producing exactly (W*H*bpp)/8 bytes.

Bit depths:
  --bpp 1  : black/white. Packed 8 px/byte, MSB = leftmost pixel,
             bit-set (1) = white, bit-clear (0) = black. This is the
             400x300 1-bpp Waveshare 4.2" format (15000 bytes).
  --bpp 4  : 6-colour Spectra E6. Packed 2 px/byte, high nibble = even
             column. This is the 1200x1600 13.3" format (960000 bytes)
             used by the sibling firmware.

The firmware embeds the output via CMake's EMBED_FILES and streams it
straight to the panel with the existing epd_display() path.

Usage (400x300 1-bpp logo splash):
    gen_splash.py --logo logo.png --out assets/splash_logo.bin \\
                  --width 400 --height 300 --bpp 1 --logo-size 200
"""
import argparse
import os
import sys
from PIL import Image, ImageDraw, ImageFont
import numpy as np

try:
    import qrcode  # only required when --qr-data is used
except ImportError:
    qrcode = None

# Order matters: tried in turn until one loads. Helvetica.ttc ships with macOS
# and renders cleanly at the sizes we use; SFNS is the system UI font.
_FONT_CANDIDATES = [
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/HelveticaNeue.ttc",
    "/System/Library/Fonts/SFNSDisplay.ttf",
    "/Library/Fonts/Arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]


def load_font(size: int) -> ImageFont.FreeTypeFont:
    for path in _FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size=size)
    # Last-resort bitmap font; small but readable.
    return ImageFont.load_default()


# Palettes keyed by name. Each entry is (packed-value, RGB). For 1-bpp the
# packed value is the bit (0=black, 1=white); for 4-bpp it's the nibble.
_PALETTES = {
    "bw": [
        (0x0, (  0,   0,   0)),   # black -> bit 0
        (0x1, (255, 255, 255)),   # white -> bit 1
    ],
    "e6": [
        (0x0, (  0,   0,   0)),   # black
        (0x1, (255, 255, 255)),   # white
        (0x2, (255, 255,   0)),   # yellow
        (0x3, (255,   0,   0)),   # red
        (0x5, (  0,   0, 255)),   # blue
        (0x6, (  0, 255,   0)),   # green
    ],
}


def make_canvas(width: int, height: int, logo_path: str,
                logo_size: int, logo_y: int) -> Image.Image:
    """Return a width x height RGB image with the logo composited on white."""
    canvas = Image.new("RGB", (width, height), (255, 255, 255))

    logo = Image.open(logo_path).convert("RGBA")
    logo = logo.resize((logo_size, logo_size), Image.LANCZOS)
    bg = Image.new("RGB", logo.size, (255, 255, 255))
    bg.paste(logo, mask=logo.split()[3])

    x = (width - logo_size) // 2
    canvas.paste(bg, (x, logo_y))
    return canvas


def overlay_labels(canvas: Image.Image, labels, y_top: int,
                   font_px: int, line_gap_px: int,
                   colour=(0, 0, 0)) -> int:
    """Centered text lines stacked vertically. Returns y of the bottom of
    the last line so callers can stack further content under them."""
    font = load_font(font_px)
    draw = ImageDraw.Draw(canvas)
    y = y_top
    for line in labels:
        bbox = draw.textbbox((0, 0), line, font=font)
        text_w = bbox[2] - bbox[0]
        x = (canvas.width - text_w) // 2
        draw.text((x, y), line, fill=colour, font=font)
        y += (bbox[3] - bbox[1]) + line_gap_px
    return y


def overlay_qr(canvas: Image.Image, data: str, target_px: int, y_top: int,
               quiet_zone: int = 4) -> int:
    """Render a QR code for `data` into a `target_px`-square area centered
    horizontally at `y_top`, scaled nearest-neighbor so every module aligns
    to an integer pixel grid (clean dither output)."""
    if qrcode is None:
        sys.exit("--qr-data requires the `qrcode` Python package "
                 "(pip install qrcode)")
    qr = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=1,
        border=quiet_zone,
    )
    qr.add_data(data)
    qr.make(fit=True)
    matrix = qr.get_matrix()    # list[list[bool]], includes quiet zone
    m_size = len(matrix)        # square; modules along one side incl. border
    module_px = target_px // m_size
    if module_px < 2:
        sys.exit(f"QR target_px={target_px} too small for {m_size}-module code "
                 f"(need at least {m_size * 2})")
    bmp_px = module_px * m_size

    bmp = Image.new("L", (bmp_px, bmp_px), 255)
    pixels = bmp.load()
    for my in range(m_size):
        for mx in range(m_size):
            if matrix[my][mx]:
                for dy in range(module_px):
                    for dx in range(module_px):
                        pixels[mx * module_px + dx, my * module_px + dy] = 0

    x = (canvas.width - bmp_px) // 2
    canvas.paste(bmp.convert("RGB"), (x, y_top))
    print(f"  qr: {m_size}x{m_size} modules ({m_size - 2 * quiet_zone} data + "
          f"{quiet_zone}-module border), {module_px}px/module, "
          f"final {bmp_px}x{bmp_px} at ({x},{y_top})")
    return y_top + bmp_px


def dither_to_indices(rgb: np.ndarray, palette_rgb: np.ndarray) -> np.ndarray:
    """Floyd-Steinberg dither rgb (H,W,3 float32) to `palette_rgb`.
    Returns an H x W int array of palette indices."""
    h, w, _ = rgb.shape
    out = np.zeros((h, w), dtype=np.int32)
    arr = rgb.copy()

    for y in range(h):
        for x in range(w):
            old = arr[y, x]
            dists = np.sum((palette_rgb - old) ** 2, axis=1)
            idx = int(np.argmin(dists))
            new = palette_rgb[idx]
            out[y, x] = idx
            err = old - new
            if x + 1 < w:
                arr[y, x + 1] += err * (7 / 16)
            if y + 1 < h:
                if x > 0:
                    arr[y + 1, x - 1] += err * (3 / 16)
                arr[y + 1, x] += err * (5 / 16)
                if x + 1 < w:
                    arr[y + 1, x + 1] += err * (1 / 16)
    return out


def pack_1bpp(indices: np.ndarray) -> bytes:
    """Pack H x W palette indices (0=black, 1=white) to 1-bpp scanline
    bytes: 8 px/byte, MSB = leftmost pixel, bit-set (1) = white."""
    bits = (indices != 0).astype(np.uint8)        # white -> 1
    packed = np.packbits(bits, axis=1)             # MSB-first, row padded
    return packed.tobytes()


def pack_4bpp(indices: np.ndarray, nibbles: np.ndarray) -> bytes:
    """Pack H x W palette indices to 4-bpp scanline bytes: high nibble =
    even column, low = odd column. `nibbles` maps index -> nibble value."""
    vals = nibbles[indices].astype(np.uint8)
    hi = vals[:, 0::2]
    lo = vals[:, 1::2]
    packed = (hi << 4) | lo
    return packed.tobytes()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--logo", required=True, help="source PNG (square logo)")
    p.add_argument("--out",  required=True, help="output .bin (panel-native)")
    p.add_argument("--width",  type=int, default=400, help="panel width px")
    p.add_argument("--height", type=int, default=300, help="panel height px")
    p.add_argument("--bpp",    type=int, default=1, choices=(1, 4),
                   help="bit depth: 1=B/W, 4=6-colour E6 (default 1)")
    p.add_argument("--palette", choices=tuple(_PALETTES.keys()),
                   help="palette name (default: bw for bpp 1, e6 for bpp 4)")
    p.add_argument("--logo-size", type=int, default=200,
                   help="logo edge in panel pixels")
    p.add_argument("--logo-y",    type=int, default=-1,
                   help="logo top Y (default: vertically centered)")
    p.add_argument("--qr-data",   help="bake a QR for this string "
                                       "(e.g. 'WIFI:T:WPA;S:...;P:...;;')")
    p.add_argument("--qr-size",   type=int, default=160,
                   help="QR target edge in panel pixels")
    p.add_argument("--qr-y",      type=int, default=72, help="QR top Y")
    p.add_argument("--label",     action="append", default=[],
                   help="centered text line under the QR (repeatable)")
    p.add_argument("--label-y",   type=int, default=-1,
                   help="top Y for the first label (default: 24px under QR)")
    p.add_argument("--label-px",  type=int, default=16, help="label font px")
    p.add_argument("--label-gap", type=int, default=6,
                   help="vertical gap between label lines (px)")
    args = p.parse_args()

    W, H = args.width, args.height
    if args.bpp == 1 and W % 8 != 0:
        sys.exit(f"--bpp 1 needs width divisible by 8 (got {W})")
    if args.bpp == 4 and W % 2 != 0:
        sys.exit(f"--bpp 4 needs even width (got {W})")

    pal_name = args.palette or ("bw" if args.bpp == 1 else "e6")
    palette = _PALETTES[pal_name]
    palette_rgb = np.array([rgb for _, rgb in palette], dtype=np.float32)
    palette_nibbles = np.array([n for n, _ in palette], dtype=np.uint8)

    if args.logo_size > W:
        sys.exit(f"logo size {args.logo_size} > panel width {W}")
    logo_y = args.logo_y if args.logo_y >= 0 else (H - args.logo_size) // 2
    if logo_y + args.logo_size > H:
        sys.exit(f"logo at y={logo_y} size {args.logo_size} exceeds height {H}")

    print(f"compositing {args.logo} size {args.logo_size}^2 on {W}x{H} white...")
    canvas = make_canvas(W, H, args.logo, args.logo_size, logo_y)

    qr_bottom = args.qr_y
    if args.qr_data:
        print(f"baking QR for {args.qr_data!r}...")
        qr_bottom = overlay_qr(canvas, args.qr_data, args.qr_size, args.qr_y)

    if args.label:
        label_y = args.label_y if args.label_y >= 0 else qr_bottom + 24
        print(f"baking {len(args.label)} label line(s) at y={label_y}...")
        overlay_labels(canvas, args.label, label_y,
                       font_px=args.label_px, line_gap_px=args.label_gap)

    print(f"Floyd-Steinberg dithering to the {pal_name} palette...")
    indices = dither_to_indices(np.array(canvas, dtype=np.float32), palette_rgb)

    if args.bpp == 1:
        packed = pack_1bpp(indices)
    else:
        packed = pack_4bpp(indices, palette_nibbles)

    expected = (W * H * args.bpp) // 8
    assert len(packed) == expected, f"got {len(packed)} bytes, expected {expected}"

    with open(args.out, "wb") as f:
        f.write(packed)
    print(f"wrote {args.out} ({len(packed)} bytes)")


if __name__ == "__main__":
    main()
