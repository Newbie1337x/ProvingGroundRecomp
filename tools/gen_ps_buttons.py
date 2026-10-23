"""Draw the PlayStation button prompts that replace the Xbox ones in the game's button font.

The art is drawn here from shapes and Pillow's built-in font (Aileron, CC0); nothing is taken
from the game. Each glyph fills the rect of the Xbox glyph it replaces in the font's 512x128
texture (see src/pg_buttons.cpp). Writes src/pg_ps_buttons.inc (a PNG as a byte array) and,
with --preview, a PNG to look at.

usage: python tools/gen_ps_buttons.py [--preview out.png]
"""
import io
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SS = 4  # supersampling

# Glyph rects in the texture, (x0, y0, x1, y1), as listed in src/pg_buttons.cpp.
RECTS = {
    'l3': (0, 0, 54, 53), 'r3': (55, 0, 109, 53),
    'triangle': (110, 0, 146, 46), 'square': (147, 0, 183, 46),
    'circle': (184, 0, 220, 46), 'cross': (221, 0, 256, 46),
    'options': (257, 0, 309, 46), 'share': (310, 0, 353, 46),
    'l': (354, 0, 408, 45), 'r': (409, 0, 463, 45),
    'l2': (464, 0, 509, 43), 'r2': (0, 54, 45, 97),
    'l1': (107, 54, 161, 91), 'r1': (162, 54, 216, 91), 'l1r1': (217, 54, 336, 91),
}

BODY = (34, 34, 38)
RIM = (12, 12, 14)
LIGHT = (92, 92, 100)
TEXT = (235, 235, 235)
SYMBOLS = {'triangle': (64, 226, 160), 'square': (232, 140, 214),
           'circle': (255, 102, 102), 'cross': (124, 178, 232)}


def font(px):
    return ImageFont.load_default(px * SS)


def canvas(rect):
    w, h = rect[2] - rect[0], rect[3] - rect[1]
    return Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))


def ball(d, cx, cy, r):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=RIM)
    r2 = r - 2.5 * SS
    d.ellipse([cx - r2, cy - r2, cx + r2, cy + r2], fill=LIGHT)
    r3 = r2 - 1.5 * SS
    d.ellipse([cx - r3, cy - r3 + SS, cx + r3, cy + r3 + SS], fill=BODY)


def text(d, cx, cy, s, px, fill=TEXT):
    d.text((cx, cy), s, font=font(px), fill=fill, anchor='mm', stroke_width=SS, stroke_fill=RIM)


def face(name, rect):
    im = canvas(rect)
    d = ImageDraw.Draw(im)
    w, h = im.size
    cx, cy, r = w / 2, h / 2, min(w, h) / 2 - SS
    ball(d, cx, cy, r)
    c = SYMBOLS[name]
    s = r * 0.45
    lw = int(3.2 * SS)
    if name == 'cross':
        d.line([cx - s, cy - s, cx + s, cy + s], fill=c, width=lw)
        d.line([cx - s, cy + s, cx + s, cy - s], fill=c, width=lw)
    elif name == 'circle':
        d.ellipse([cx - s, cy - s, cx + s, cy + s], outline=c, width=lw)
    elif name == 'square':
        s *= 0.85
        d.rectangle([cx - s, cy - s, cx + s, cy + s], outline=c, width=lw)
    else:
        s *= 1.1
        pts = [(cx, cy - s), (cx + s * 0.95, cy + s * 0.65), (cx - s * 0.95, cy + s * 0.65)]
        d.line(pts + [pts[0], pts[1]], fill=c, width=lw, joint='curve')
    return im


def stick(label, rect, pressed):
    im = canvas(rect)
    d = ImageDraw.Draw(im)
    w, h = im.size
    # Base of the stick, then the cap.
    d.ellipse([w * 0.2, h * 0.55, w * 0.8, h - SS], fill=RIM)
    d.ellipse([w * 0.24, h * 0.58, w * 0.76, h - 3 * SS], fill=BODY)
    cap = [w * 0.08, SS, w * 0.92, h * 0.72]
    d.ellipse(cap, fill=RIM)
    d.ellipse([cap[0] + 2.5 * SS, cap[1] + 2.5 * SS, cap[2] - 2.5 * SS, cap[3] - 2.5 * SS], fill=LIGHT)
    d.ellipse([cap[0] + 4 * SS, cap[1] + 5 * SS, cap[2] - 4 * SS, cap[3] - 3 * SS], fill=BODY)
    text(d, w / 2, (cap[1] + cap[3]) / 2 + SS, label, 20 if pressed else 22)
    return im


def pill(label, rect, px=17):
    im = canvas(rect)
    d = ImageDraw.Draw(im)
    w, h = im.size
    d.rounded_rectangle([SS, SS, w - SS, h - SS], radius=h / 2.4, fill=RIM)
    d.rounded_rectangle([3 * SS, 3 * SS, w - 3 * SS, h - 3 * SS], radius=h / 2.4, fill=LIGHT)
    d.rounded_rectangle([4.5 * SS, 5 * SS, w - 4.5 * SS, h - 4 * SS], radius=h / 2.4, fill=BODY)
    text(d, w / 2, h / 2 + SS * 0.5, label, px)
    return im


def trigger(label, rect):
    im = canvas(rect)
    d = ImageDraw.Draw(im)
    w, h = im.size
    # A trigger seen from the front: rounded top, narrower bottom.
    pts = [(w * 0.1, h * 0.3), (w * 0.3, SS), (w * 0.7, SS), (w * 0.9, h * 0.3),
           (w * 0.82, h - SS), (w * 0.18, h - SS)]
    d.polygon(pts, fill=RIM)
    d.polygon([(x + (w / 2 - x) * 0.1, y + (h / 2 - y) * 0.1) for x, y in pts], fill=LIGHT)
    d.polygon([(x + (w / 2 - x) * 0.16, y + (h / 2 - y) * 0.12 + SS) for x, y in pts], fill=BODY)
    text(d, w / 2, h / 2 + SS, label, 19)
    return im


def menu_button(label, rect, icon):
    im = canvas(rect)
    d = ImageDraw.Draw(im)
    w, h = im.size
    bw, bh = w * 0.62, h * 0.42
    x0, y0 = (w - bw) / 2, 2 * SS
    d.rounded_rectangle([x0, y0, x0 + bw, y0 + bh], radius=bh / 2, fill=RIM)
    d.rounded_rectangle([x0 + 2 * SS, y0 + 2 * SS, x0 + bw - 2 * SS, y0 + bh - 2 * SS],
                        radius=bh / 2, fill=BODY)
    cx, cy = w / 2, y0 + bh / 2
    lw = int(1.6 * SS)
    if icon == 'lines':
        for k in (-1, 0, 1):
            y = cy + k * bh * 0.2
            d.line([cx - bw * 0.2, y, cx + bw * 0.2, y], fill=TEXT, width=lw)
    else:
        # Share: an arrow pointing up.
        s = bh * 0.28
        d.line([cx, cy + s, cx, cy - s], fill=TEXT, width=lw)
        d.line([cx - s * 0.7, cy - s * 0.3, cx, cy - s, cx + s * 0.7, cy - s * 0.3], fill=TEXT, width=lw)
    d.text((w / 2, h - 2 * SS), label, font=font(10.5 if len(label) > 5 else 12), fill=TEXT,
           anchor='mb', stroke_width=SS, stroke_fill=RIM)
    return im


def draw():
    atlas = Image.new('RGBA', (512, 128), (0, 0, 0, 0))
    glyphs = {name: face(name, RECTS[name]) for name in SYMBOLS}
    glyphs.update(l3=stick('L3', RECTS['l3'], True), r3=stick('R3', RECTS['r3'], True),
                  l=stick('L', RECTS['l'], False), r=stick('R', RECTS['r'], False),
                  l2=trigger('L2', RECTS['l2']), r2=trigger('R2', RECTS['r2']),
                  l1=pill('L1', RECTS['l1']), r1=pill('R1', RECTS['r1']),
                  l1r1=pill('L1 + R1', RECTS['l1r1']),
                  options=menu_button('OPTIONS', RECTS['options'], 'lines'),
                  share=menu_button('SHARE', RECTS['share'], 'share'))
    for name, im in glyphs.items():
        x0, y0, x1, y1 = RECTS[name]
        atlas.paste(im.resize((x1 - x0, y1 - y0), Image.LANCZOS), (x0, y0))
    return atlas


def main():
    atlas = draw()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    png = io.BytesIO()
    atlas.save(png, 'PNG', optimize=True)
    data = png.getvalue()
    lines = ['// Generated by tools/gen_ps_buttons.py: PlayStation button prompts (PNG, 512x128).']
    for i in range(0, len(data), 24):
        lines.append(''.join('0x%02X,' % b for b in data[i:i + 24]))
    with open(os.path.join(root, 'src', 'pg_ps_buttons.inc'), 'w', newline='\n') as out:
        out.write('\n'.join(lines) + '\n')
    if '--preview' in sys.argv:
        bg = Image.new('RGBA', atlas.size, (90, 90, 90, 255))
        bg.alpha_composite(atlas)
        bg.resize((1024, 256), Image.NEAREST).save(sys.argv[sys.argv.index('--preview') + 1])
    print(len(data), 'bytes')


if __name__ == '__main__':
    main()
