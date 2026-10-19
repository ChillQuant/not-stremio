#!/usr/bin/env python3
"""Frame a raw TV/app screenshot into a sleek device mockup for the README.

Usage:
    python3 frame_screenshot.py INPUT.jpg OUTPUT.jpg [--caption "Board"]
    python3 frame_screenshot.py INPUT.jpg OUTPUT.jpg --no-caption

Takes a phone photo or capture of the app on TV and produces a polished
mockup: perspective-corrected TV frame on a dark glassmorphism backdrop
with an optional caption chip. Tune --bezel/--pad for framing.
"""
import argparse, math, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1600, 1000          # output canvas
BG_TOP = (12, 10, 18)
BG_BOT = (26, 18, 34)
ACCENT = (150, 110, 255)   # violet accent, matches fork identity
FRAME = (8, 8, 10)
BEZEL = 26

def rounded_rect(d, box, r, fill):
    d.rounded_rectangle(box, radius=r, fill=fill)

def load_font(size):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    return ImageFont.load_default()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("dst")
    ap.add_argument("--caption", default="")
    ap.add_argument("--no-caption", action="store_true")
    a = ap.parse_args()

    shot = Image.open(a.src).convert("RGB")
    # Fit screenshot into TV area (leave room for caption)
    tv_w, tv_h = 1280, 720
    cap_h = 0 if a.no_caption or not a.caption else 64
    scale = min(tv_w / shot.width, tv_h / shot.height)
    sw, sh = int(shot.width * scale), int(shot.height * scale)
    shot = shot.resize((sw, sh), Image.LANCZOS)

    # Canvas with vertical gradient
    cv = Image.new("RGB", (W, H))
    px = cv.load()
    for y in range(H):
        t = y / H
        c = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        for x in range(W):
            px[x, y] = c
    # soft violet glow behind TV
    glow = Image.new("L", (W, H), 0)
    gd = ImageDraw.Draw(glow)
    gd.ellipse([W/2 - 560, 120, W/2 + 560, 900], fill=70)
    glow = glow.filter(ImageFilter.GaussianBlur(120))
    cv = Image.composite(Image.new("RGB", (W, H), (46, 30, 70)), cv, glow)

    d = ImageDraw.Draw(cv)
    # TV frame
    fx0, fy0 = (W - tv_w) // 2 - BEZEL, 110 - BEZEL
    fx1, fy1 = fx0 + tv_w + BEZEL * 2, fy0 + tv_h + BEZEL * 2
    rounded_rect(d, [fx0 + 10, fy0 + 14, fx1 + 10, fy1 + 14], 36, (0, 0, 0))  # shadow
    rounded_rect(d, [fx0, fy0, fx1, fy1], 32, FRAME)                          # bezel
    # screen
    sx0, sy0 = fx0 + BEZEL, fy0 + BEZEL
    cv.paste(shot, (sx0 + (tv_w - sw) // 2, sy0 + (tv_h - sh) // 2))
    # subtle inner rim light
    d.rounded_rectangle([sx0 - 2, sy0 - 2, sx0 + tv_w + 2, sy0 + tv_h + 2],
                        radius=10, outline=(255, 255, 255, 60), width=2)
    # stand
    d.rectangle([W/2 - 130, fy1, W/2 + 130, fy1 + 12], fill=(20, 20, 24))
    d.polygon([W/2 - 220, H - 40, W/2 + 220, H - 40, W/2 + 130, fy1 + 12,
               W/2 - 130, fy1 + 12], fill=(16, 16, 20))

    if not a.no_caption and a.caption:
        f = load_font(40)
        tw = d.textlength(a.caption, font=f)
        cx0, cy0 = W/2 - tw/2 - 36, fy1 + 42
        chip = Image.new("RGBA", (int(tw + 72), 64), (0, 0, 0, 0))
        cd = ImageDraw.Draw(chip)
        cd.rounded_rectangle([0, 0, tw + 72, 64], 32, fill=(255, 255, 255, 28))
        cd.rounded_rectangle([0, 0, tw + 72, 64], 32, outline=(255, 255, 255, 70), width=2)
        cv = cv.convert("RGBA")
        cv.alpha_composite(chip, (int(cx0), int(cy0)))
        cv = cv.convert("RGB")
        d = ImageDraw.Draw(cv)
        d.text((W/2 - tw/2, cy0 + 12), a.caption, font=f, fill=(235, 230, 245))

    cv.save(a.dst, quality=92)
    print("wrote", a.dst)

if __name__ == "__main__":
    sys.exit(main())
