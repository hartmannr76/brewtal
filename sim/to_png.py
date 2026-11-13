#!/usr/bin/env python3
"""Convert sim RGB565 dumps to PNGs (round-masked, on a bezel) + a contact sheet."""
import glob
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SIZE = 360  # replaced per file from the dump size


def load(path):
    global SIZE
    data = open(path, "rb").read()
    SIZE = int(round((len(data) // 2) ** 0.5))
    img = Image.new("RGB", (SIZE, SIZE))
    px = img.load()
    for i in range(SIZE * SIZE):
        v = data[2 * i] | (data[2 * i + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        px[i % SIZE, i // SIZE] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    return img


def framed(img):
    pad = 26
    s = SIZE + 2 * pad
    out = Image.new("RGB", (s, s), (245, 245, 243))
    d = ImageDraw.Draw(out)
    d.ellipse([0, 0, s - 1, s - 1], fill=(29, 32, 36))
    d.ellipse([pad - 8, pad - 8, s - pad + 7, s - pad + 7], fill=(5, 6, 7))
    mask = Image.new("L", (SIZE * 4, SIZE * 4), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, SIZE * 4 - 1, SIZE * 4 - 1], fill=255)
    mask = mask.resize((SIZE, SIZE), Image.LANCZOS)
    out.paste(img, (pad, pad), mask)
    return out


def main(d):
    files = sorted(glob.glob(os.path.join(d, "*.raw")))
    tiles = []
    for f in files:
        name = os.path.splitext(os.path.basename(f))[0]
        img = framed(load(f))
        img.save(os.path.join(d, name + ".png"))
        tiles.append((name, img))
    if not tiles:
        return
    cols = 4
    tw, th = tiles[0][1].size
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * tw, rows * (th + 30)), (245, 245, 243))
    dr = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 16)
    except OSError:
        font = ImageFont.load_default()
    for i, (name, img) in enumerate(tiles):
        x, y = (i % cols) * tw, (i // cols) * (th + 30)
        sheet.paste(img, (x, y))
        dr.text((x + 14, y + th + 4), name.replace("_", " "), fill=(60, 60, 60), font=font)
    sheet.save(os.path.join(d, "contact_sheet.png"))
    print("wrote", len(tiles), "frames + contact_sheet.png")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out")
