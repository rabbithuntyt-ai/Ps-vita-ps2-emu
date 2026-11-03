#!/usr/bin/env python3
"""VitaStation2 branding: the wordmark and every image made from it.

    python3 tools/make_branding.py

Writes
  sce_sys/icon0.png, sce_sys/livearea/contents/bg.png, startup.png
      (8-bit palette PNGs, as the Vita requires)
  assets/brand/wordmark.a8, digit.a8, glow.a8
      alpha masks the app tints and draws (8 byte header: width, height as
      little-endian uint32, then one byte per pixel)
  docs/branding_preview.png
      everything side by side, for review

The wordmark is original lettering drawn from strokes and arcs: wide, thin,
geometric letters in the style of early-2000s console branding. It is not a
font or logo from any console maker.
"""
import os
import struct
from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SS = 4  # supersampling for anti-aliased strokes

# ---------------------------------------------------------------- lettering
# Glyphs in font units: cap height 100, x-height 72, baseline at y = 100.
CAP, XH, STROKE = 100.0, 72.0, 8.5
GAP = 15.0


def glyph_width(c):
    return {"V": 92, "i": 0, "t": 34, "a": XH + 6, "S": 74, "o": XH + 4, "n": XH - 4, "2": 66}[c]


def draw_glyph(d, c, x0, s, ox, oy):
    """Draws glyph c with its left edge at x0 (font units), scale s."""
    w = STROKE * s

    def P(x, y):
        return (ox + (x0 + x) * s, oy + y * s)

    def line(a, b):
        d.line([P(*a), P(*b)], fill=255, width=int(round(w)))
        for p in (a, b):  # round caps
            cx, cy = P(*p)
            d.ellipse([cx - w / 2, cy - w / 2, cx + w / 2, cy + w / 2], fill=255)

    def arc(box, start, end):
        x1, y1, x2, y2 = box
        (ax, ay), (bx, by) = P(x1, y1), P(x2, y2)
        d.arc([ax, ay, bx, by], start, end, fill=255, width=int(round(w)))

    def dot(x, y, r):
        cx, cy = P(x, y)
        d.ellipse([cx - r * s, cy - r * s, cx + r * s, cy + r * s], fill=255)

    h = STROKE / 2
    top = CAP - XH
    if c == "V":
        line((h, h), (46, CAP - h))
        line((46, CAP - h), (92 - h, h))
    elif c == "i":
        line((0, top + h), (0, CAP - h))
        dot(0, top - 17, 5.5)
    elif c == "t":
        line((12, top - 22), (12, CAP - 18))
        arc((12, CAP - 36, 48, CAP - h), 90, 180)  # foot curving right
        line((0, top + h), (32, top + h))
    elif c == "a":
        arc((h, top + h, XH - h, CAP - h), 0, 360)
        line((XH + 2, top + h), (XH + 2, CAP - h))
    elif c == "o":
        arc((h, top + h, XH + 4 - h, CAP - h), 0, 360)
    elif c == "n":
        line((h, top + h), (h, CAP - h))
        arc((h, top + h, XH - 4 - h, top + XH - 4), 180, 360)
        line((XH - 4 - h, top + (XH - 4) / 2), (XH - 4 - h, CAP - h))
    elif c == "S":
        mid = CAP / 2
        arc((h, h, 74 - h, mid + h / 2), 90, 330)
        arc((h, mid - h / 2, 74 - h, CAP - h), 270, 510)
    elif c == "2":
        arc((h, h, 66 - h, 56), 180, 375)
        line((64, 34), (h, CAP - h))
        line((h, CAP - h), (66 - h, CAP - h))


def text_width(text):
    return sum(glyph_width(c) for c in text) + GAP * (len(text) - 1)


def render_mask(text, height, pad=0.7):
    """Alpha mask of text, cap height 'height' pixels, with padding for a glow."""
    s = height / CAP * SS
    p = int(height * pad * SS)
    top_extra = 25 * s  # i dots and t ascenders above the cap line
    w = int(text_width(text) * s + STROKE * s + 2 * p)
    hpx = int(CAP * s + top_extra + STROKE * s + 2 * p)
    img = Image.new("L", (w, hpx), 0)
    d = ImageDraw.Draw(img)
    x = 0.0
    for c in text:
        draw_glyph(d, c, x + STROKE / 2, s, p, p + top_extra)
        x += glyph_width(c) + GAP
    return img.resize((w // SS, hpx // SS), Image.LANCZOS)


def wordmark(height):
    """'VitaStation' and '2' masks with the same metrics, side by side."""
    full = render_mask("VitaStation2", height)
    name = render_mask("VitaStation", height)
    # The digit is the part of the full mark past the name.
    digit = full.copy()
    d = ImageDraw.Draw(digit)
    split = name.width - int(height * 0.7) + int(GAP * height / CAP / 2)
    d.rectangle([0, 0, split, digit.height], fill=0)
    name_full = full.copy()
    d = ImageDraw.Draw(name_full)
    d.rectangle([split, 0, full.width, full.height], fill=0)
    return name_full, digit, full


# ---------------------------------------------------------------- colors
NAVY = (6, 10, 34)
DEEP = (12, 26, 82)
BLUE = (40, 110, 255)
CYAN = (110, 200, 255)
WHITE = (240, 244, 255)


def gradient(size, top, bottom):
    w, h = size
    img = Image.new("RGB", size)
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def glow_spot(img, center, radius, color, strength):
    w, h = img.size
    spot = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(spot)
    cx, cy = center
    d.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], fill=int(255 * strength))
    spot = spot.filter(ImageFilter.GaussianBlur(radius * 0.6))
    layer = Image.new("RGB", (w, h), color)
    return Image.composite(layer, img, spot)


def light_beams(img, count, color, strength):
    """Soft diagonal light streaks."""
    w, h = img.size
    beams = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(beams)
    for i in range(count):
        x = w * (0.15 + 0.8 * i / max(1, count - 1))
        d.polygon([(x, -10), (x + w * 0.05, -10), (x - w * 0.25, h + 10), (x - w * 0.31, h + 10)], fill=int(255 * strength * (0.5 + 0.5 * (i % 2))))
    beams = beams.filter(ImageFilter.GaussianBlur(w * 0.02))
    layer = Image.new("RGB", (w, h), color)
    return Image.composite(layer, img, beams)


def stamp(img, mask, pos, color, glow=None):
    """Composites a tinted mask (with an optional blurred glow under it)."""
    if glow:
        gcolor, gradius, gstrength = glow
        g = mask.filter(ImageFilter.GaussianBlur(gradius)).point(lambda v: min(255, int(v * gstrength)))
        full = Image.new("L", img.size, 0)
        full.paste(g, pos)
        img = Image.composite(Image.new("RGB", img.size, gcolor), img, full)
    full = Image.new("L", img.size, 0)
    full.paste(mask, pos)
    return Image.composite(Image.new("RGB", img.size, color), img, full)


def stamp_mark(img, height, center, glow_radius):
    name, digit, full = wordmark(height)
    x = int(center[0] - full.width / 2)
    y = int(center[1] - full.height / 2)
    img = stamp(img, full, (x, y), CYAN, glow=(BLUE, glow_radius, 1.6))
    img = stamp(img, name, (x, y), WHITE)
    img = stamp(img, digit, (x, y), CYAN)
    return img


def save_palette_png(img, path):
    img.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(path, optimize=True)


def save_mask(mask, path):
    with open(path, "wb") as f:
        f.write(struct.pack("<II", mask.width, mask.height))
        f.write(mask.tobytes())


def subtitle(img, text, y, size, color):
    from PIL import ImageFont
    font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", size)
    d = ImageDraw.Draw(img)
    tw = d.textlength(text, font=font)
    # Letter-spaced, light: matches the wordmark's wide look.
    spaced = " ".join(text)
    tw = d.textlength(spaced, font=font)
    d.text(((img.width - tw) / 2, y), spaced, font=font, fill=color)
    return img


def main():
    os.makedirs(os.path.join(ROOT, "assets", "brand"), exist_ok=True)
    live = os.path.join(ROOT, "sce_sys", "livearea", "contents")

    # LiveArea background 840x500
    bg = gradient((840, 500), DEEP, NAVY)
    bg = light_beams(bg, 5, (30, 70, 170), 0.35)
    bg = glow_spot(bg, (420, 210), 230, (24, 60, 170), 0.55)
    bg = stamp_mark(bg, 64, (420, 200), 14)
    bg = subtitle(bg, "PLAYSTATION 2 EMULATOR FOR PS VITA", 268, 15, (150, 175, 230))
    save_palette_png(bg, os.path.join(live, "bg.png"))

    # Gate (startup) image 280x158
    gate = gradient((280, 158), (18, 40, 120), NAVY)
    gate = glow_spot(gate, (140, 70), 90, (30, 80, 210), 0.6)
    gate = stamp_mark(gate, 26, (140, 68), 6)
    gate = subtitle(gate, "START", 112, 12, (190, 210, 255))
    save_palette_png(gate, os.path.join(live, "startup.png"))

    # Icon 128x128: "VS2" monogram on a glowing disc
    icon = gradient((128, 128), (20, 50, 150), NAVY)
    icon = glow_spot(icon, (64, 64), 54, (40, 110, 255), 0.7)
    ring = Image.new("L", (128 * SS, 128 * SS), 0)
    ImageDraw.Draw(ring).ellipse([10 * SS, 10 * SS, 118 * SS, 118 * SS], outline=255, width=3 * SS)
    ring = ring.resize((128, 128), Image.LANCZOS)
    icon = Image.composite(Image.new("RGB", icon.size, CYAN), icon, ring)
    vs = render_mask("VS", 30, pad=0)
    two = render_mask("2", 30, pad=0)
    total = vs.width + two.width - 4
    icon = stamp(icon, vs, (64 - total // 2, 64 - vs.height // 2 + 3), WHITE, glow=(BLUE, 4, 1.4))
    icon = stamp(icon, two, (64 - total // 2 + vs.width - 4, 64 - two.height // 2 + 3), CYAN)
    save_palette_png(icon, os.path.join(ROOT, "sce_sys", "icon0.png"))

    # In-app masks: header wordmark (cap height 34 px on the 960x544 screen)
    name, digit, full = wordmark(34)
    save_mask(name, os.path.join(ROOT, "assets", "brand", "wordmark.a8"))
    save_mask(digit, os.path.join(ROOT, "assets", "brand", "digit.a8"))
    save_mask(full.filter(ImageFilter.GaussianBlur(7)).point(lambda v: min(255, v * 2)), os.path.join(ROOT, "assets", "brand", "glow.a8"))

    # Button icons for hints, 4 x 32 px in a row: cross, circle, triangle, square
    icons = Image.new("L", (4 * 32 * SS, 32 * SS), 0)
    d = ImageDraw.Draw(icons)
    sw = int(3.2 * SS)
    for i in range(4):
        x0, c = i * 32 * SS, 16 * SS
        m = 7 * SS
        if i == 0:
            d.line([(x0 + c - m, c - m), (x0 + c + m, c + m)], fill=255, width=sw)
            d.line([(x0 + c - m, c + m), (x0 + c + m, c - m)], fill=255, width=sw)
        elif i == 1:
            d.ellipse([x0 + c - m - SS, c - m - SS, x0 + c + m + SS, c + m + SS], outline=255, width=sw)
        elif i == 2:
            pts = [(x0 + c, c - m - 2 * SS), (x0 + c + m + 2 * SS, c + m), (x0 + c - m - 2 * SS, c + m)]
            d.line(pts + [pts[0], pts[1]], fill=255, width=sw, joint="curve")
        else:
            d.rectangle([x0 + c - m, c - m, x0 + c + m, c + m], outline=255, width=sw)
    save_mask(icons.resize((4 * 32, 32), Image.LANCZOS), os.path.join(ROOT, "assets", "brand", "buttons.a8"))

    # Preview sheet
    sheet = Image.new("RGB", (840 + 20 + 280, 520), (30, 30, 30))
    sheet.paste(Image.open(os.path.join(live, "bg.png")).convert("RGB"), (0, 0))
    sheet.paste(Image.open(os.path.join(live, "startup.png")).convert("RGB"), (860, 0))
    sheet.paste(Image.open(os.path.join(ROOT, "sce_sys", "icon0.png")).convert("RGB"), (860, 180))
    sheet.save(os.path.join(ROOT, "docs", "branding_preview.png"))
    print("branding written")


if __name__ == "__main__":
    main()
