# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: draws the launcher icon (original art, no game assets): make_icon.py <out without extension>"""
from PIL import Image, ImageDraw, ImageFilter
import math, sys
S = 1024
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
# Sky: vertical gradient, stormy grey-blue to warm horizon
sky = Image.new('RGB', (S, S))
px = sky.load()
for y in range(S):
    t = y / S
    top = (38, 46, 56); mid = (120, 118, 108); low = (210, 186, 140)
    if t < 0.62:
        k = t / 0.62; c = tuple(int(top[i] + (mid[i] - top[i]) * k) for i in range(3))
    else:
        k = (t - 0.62) / 0.38; c = tuple(int(mid[i] + (low[i] - mid[i]) * k) for i in range(3))
    for x in range(S):
        px[x, y] = c
d = ImageDraw.Draw(sky)
# Distant cliffs
d.polygon([(0, 760), (120, 700), (230, 730), (330, 670), (420, 720), (520, 690), (640, 735), (760, 680), (880, 720), (1024, 690), (1024, 1024), (0, 1024)], fill=(92, 84, 72))
# Colossus silhouette (walking giant, broad shoulders, stone body)
col = (24, 26, 28)
d.polygon([(520, 250), (575, 240), (610, 270), (612, 320), (590, 345), (680, 380), (735, 470), (760, 600), (730, 610), (700, 520), (690, 610), (705, 760), (660, 770), (630, 640), (600, 640), (585, 770), (535, 770), (545, 620), (520, 520), (470, 600), (430, 590), (470, 470), (520, 380), (505, 330), (500, 280)], fill=col)
# Ground
d.polygon([(0, 790), (300, 770), (560, 785), (800, 765), (1024, 780), (1024, 1024), (0, 1024)], fill=(46, 42, 36))
# Wander: tiny figure holding the sword up, lower left
d.rectangle([248, 700, 262, 760], fill=(18, 18, 18))
d.ellipse([244, 684, 266, 704], fill=(18, 18, 18))
d.line([(258, 705), (275, 640)], fill=(18, 18, 18), width=7)
# Light beam from the sword toward the colossus
beam = Image.new('RGBA', (S, S), (0, 0, 0, 0))
bd = ImageDraw.Draw(beam)
x0, y0, x1, y1 = 276, 636, 590, 330
for w, a in ((60, 30), (32, 60), (14, 140), (5, 255)):
    bd.line([(x0, y0), (x1, y1)], fill=(255, 246, 214, a), width=w)
beam = beam.filter(ImageFilter.GaussianBlur(6))
sky = Image.alpha_composite(sky.convert('RGBA'), beam)
bd2 = ImageDraw.Draw(sky)
bd2.line([(x0, y0), (x1, y1)], fill=(255, 252, 235, 255), width=4)
# Rounded square mask with thin frame
mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([16, 16, S - 16, S - 16], radius=180, fill=255)
img.paste(sky, (0, 0), mask)
ImageDraw.Draw(img).rounded_rectangle([16, 16, S - 16, S - 16], radius=180, outline=(215, 192, 138, 255), width=14)
out = sys.argv[1]
img.resize((256, 256), Image.LANCZOS).save(out + '.png')
img.save(out + '.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
img.resize((512, 512), Image.LANCZOS).save(out + '-512.png')
