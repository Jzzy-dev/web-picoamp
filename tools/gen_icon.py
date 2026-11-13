#!/usr/bin/env python3
# res/picoamp.ico: PICOAMP's spectrum bars on navy, in PICOAMP's own colours,
# drawn on a 16 x 16 grid and blown up without smoothing (crisp MSX pixels).
#
#   python3 tools/gen_icon.py      (needs Pillow)
import os
from PIL import Image

def p3(r, g, b):
    return (r * 255 // 7, g * 255 // 7, b * 255 // 7, 255)

NAVY, BLUE = p3(0, 0, 2), p3(1, 1, 3)
GREEN, YELLOW, RED, WHITE = p3(2, 7, 2), p3(7, 7, 1), p3(7, 1, 1), p3(7, 7, 7)
CLEAR = (0, 0, 0, 0)

# bar heights (in rows of the 16 x 16 grid), peaks one row higher
bars = [5, 9, 12, 7, 10, 6]

def grid():
    g = [[NAVY] * 16 for _ in range(16)]
    # rounded corners
    for (x, y) in [(0, 0), (15, 0), (0, 15), (15, 15)]:
        g[y][x] = CLEAR
    # a title bar row, as on the screen
    for x in range(1, 15):
        g[1][x] = BLUE
    g[1][14] = RED
    for i, h in enumerate(bars):
        x = 2 + i * 2
        for k in range(h):
            y = 14 - k
            seg = k
            c = GREEN if seg < 7 else YELLOW if seg < 10 else RED
            if y > 2:
                g[y][x] = c
        py = 14 - h - 1
        if py > 2:
            g[py][x] = WHITE
    return g

def render(size):
    g = grid()
    small = Image.new('RGBA', (16, 16))
    for y in range(16):
        for x in range(16):
            small.putpixel((x, y), g[y][x])
    return small.resize((size, size), Image.NEAREST)

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, '..', 'res', 'picoamp.ico')
sizes = [16, 24, 32, 48, 64, 128, 256]
imgs = [render(s) for s in sizes]
imgs[-1].save(out, format='ICO', sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
render(256).save(os.path.join(here, '..', 'res', 'picoamp_icon.png'))
print('wrote', out)

# the MSIX package's pictures (msix/Assets): the icon at whole multiples of its
# 16 x 16 grid, in the middle of each size (transparent around it)
assets = os.path.join(here, '..', 'msix', 'Assets')
os.makedirs(assets, exist_ok=True)

def placed(w, h, fill=CLEAR):
    k = max(1, min(w, h) // 16)
    pic = render(16 * k)
    img = Image.new('RGBA', (w, h), fill)
    img.paste(pic, ((w - 16 * k) // 2, (h - 16 * k) // 2), pic)
    return img

for name, w, h in [('Square44x44Logo', 44, 44), ('Square150x150Logo', 150, 150), ('Wide310x150Logo', 310, 150),
                   ('SmallTile', 71, 71), ('LargeTile', 310, 310), ('StoreLogo', 50, 50), ('SplashScreen', 620, 300)]:
    placed(w, h).save(os.path.join(assets, name + '.png'))
for t in (16, 24, 32, 48, 256):
    placed(t, t).save(os.path.join(assets, 'Square44x44Logo.targetsize-%d.png' % t))
    placed(t, t).save(os.path.join(assets, 'Square44x44Logo.targetsize-%d_altform-unplated.png' % t))
print('wrote', assets)
