# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: draws the launcher icon (original art, no game assets): make_icon.py <out without extension>

A colossus (hunched stone giant with fur on its legs, a glowing sigil on its head) under a
stormy sky; Wander with his cape, sword raised, the sword's beam of light on the sigil; Agro
beside him on the ridge. Written as plain shapes so it stays readable down to 16x16."""
from PIL import Image, ImageDraw, ImageFilter
import random
import sys

S = 1024
random.seed(7)


def gradient(size, stops):
    """Vertical gradient through (position 0..1, rgb) stops."""
    img = Image.new('RGB', size)
    px = img.load()
    w, h = size
    for y in range(h):
        t = y / (h - 1)
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if p0 <= t <= p1:
                k = (t - p0) / (p1 - p0)
                c = tuple(int(c0[i] + (c1[i] - c0[i]) * k) for i in range(3))
                break
        for x in range(w):
            px[x, y] = c
    return img


# Sky: storm above, pale light near the horizon behind the colossus
art = gradient((S, S), [(0.0, (22, 27, 34)), (0.35, (58, 64, 70)), (0.68, (150, 146, 128)),
                        (1.0, (196, 178, 140))]).convert('RGBA')
glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
ImageDraw.Draw(glow).ellipse([380, 330, 980, 900], fill=(255, 236, 196, 90))
art = Image.alpha_composite(art, glow.filter(ImageFilter.GaussianBlur(90)))

# Clouds: soft dark bands
clouds = Image.new('RGBA', (S, S), (0, 0, 0, 0))
cd = ImageDraw.Draw(clouds)
for _ in range(26):
    x = random.randint(-200, S)
    y = random.randint(40, 420)
    w = random.randint(240, 520)
    h = random.randint(40, 90)
    cd.ellipse([x, y, x + w, y + h], fill=(14, 17, 22, random.randint(60, 120)))
art = Image.alpha_composite(art, clouds.filter(ImageFilter.GaussianBlur(28)))

d = ImageDraw.Draw(art)
# Distant mesas
d.polygon([(0, 735), (90, 700), (150, 712), (210, 650), (300, 655), (330, 700), (420, 690),
           (470, 640), (560, 646), (600, 700), (700, 690), (760, 655), (850, 660), (900, 700),
           (1024, 690), (1024, 1024), (0, 1024)], fill=(104, 96, 82))

# --- The colossus (right): hunched stone giant, fur on legs and back ---
STONE = (52, 50, 48)
STONE_LIGHT = (86, 82, 76)
FUR = (30, 28, 26)
# fur mass (legs and back), jagged outline
fur = [(560, 470), (590, 420), (640, 380), (700, 360), (760, 370), (812, 410), (840, 470),
       (850, 560), (842, 640), (858, 720), (850, 790), (815, 800), (800, 730), (782, 790),
       (742, 800), (735, 720), (700, 712), (690, 790), (650, 800), (640, 730), (618, 795),
       (585, 790), (592, 700), (575, 620), (560, 540)]
jag = []
for (x0, y0), (x1, y1) in zip(fur, fur[1:] + fur[:1]):
    jag.append((x0, y0))
    for k in range(1, 4):
        t = k / 4
        jag.append((x0 + (x1 - x0) * t + random.randint(-9, 9), y0 + (y1 - y0) * t + random.randint(-9, 9)))
d.polygon(jag, fill=FUR)
# stone armor plates: chest, shoulders, shins
d.polygon([(600, 450), (690, 420), (780, 440), (800, 520), (760, 590), (650, 600), (605, 540)], fill=STONE)
d.polygon([(620, 470), (690, 445), (760, 462), (772, 520), (690, 535)], fill=STONE_LIGHT)
d.polygon([(585, 690), (650, 680), (655, 770), (598, 778)], fill=STONE)
d.polygon([(745, 690), (810, 682), (822, 772), (760, 780)], fill=STONE)
# arms: left arm hanging with a stone club
d.polygon([(600, 460), (560, 520), (535, 610), (520, 690), (548, 700), (575, 615), (610, 540)], fill=FUR)
d.polygon([(500, 680), (560, 672), (568, 760), (492, 772)], fill=STONE)          # club
d.polygon([(800, 455), (845, 520), (868, 610), (850, 640), (825, 560), (790, 510)], fill=FUR)
# head: small, low between the shoulders, with a glowing sigil
d.polygon([(645, 360), (690, 330), (735, 338), (750, 380), (722, 410), (668, 410)], fill=STONE)
d.ellipse([672, 368, 684, 380], fill=(150, 210, 255))   # eyes
d.ellipse([706, 368, 718, 380], fill=(150, 210, 255))
# crack lines on the stone
for (a, b) in [((640, 500), (700, 515)), ((700, 515), (740, 560)), ((610, 720), (640, 740)),
               ((770, 720), (800, 745))]:
    d.line([a, b], fill=(34, 33, 32), width=4)

# --- Ground ridge in front ---
d.polygon([(0, 800), (160, 778), (330, 770), (520, 790), (700, 806), (880, 798), (1024, 808),
           (1024, 1024), (0, 1024)], fill=(40, 36, 31))
d.polygon([(0, 860), (220, 842), (430, 850), (640, 870), (1024, 860), (1024, 1024), (0, 1024)], fill=(30, 27, 23))

# --- Agro (horse) beside Wander, left ---
H = (16, 15, 14)
d.polygon([(140, 702), (248, 694), (270, 712), (266, 740), (150, 746), (136, 726)], fill=H)  # body
d.polygon([(244, 700), (270, 650), (284, 628), (300, 622), (322, 640), (318, 652), (300, 654),
           (288, 676), (268, 716)], fill=H)                                               # neck, head
d.polygon([(290, 624), (294, 608), (302, 620)], fill=H)                                     # ear
for x in (150, 166, 236, 252):                                                              # legs
    d.polygon([(x, 738), (x + 9, 738), (x + 8, 796), (x + 1, 796)], fill=H)
d.line([(140, 708), (120, 764)], fill=H, width=8)                                           # tail
d.line([(250, 650), (238, 690)], fill=(10, 10, 10), width=5)                                # mane

# --- Wander: cape, sword raised ---
W = (12, 12, 12)
d.ellipse([343, 676, 365, 698], fill=W)                                          # head
d.polygon([(342, 700), (366, 700), (372, 750), (362, 790), (350, 790), (344, 752), (330, 760)], fill=W)
d.polygon([(342, 704), (318, 760), (334, 768), (348, 730)], fill=(22, 22, 24))   # cape
d.line([(362, 706), (384, 660)], fill=W, width=8)                                # arm up
SWORD_TIP = (398, 620)
d.line([(384, 662), SWORD_TIP], fill=(210, 214, 220), width=6)

# --- The sword's beam on the colossus's sigil ---
SIGIL = (697, 352)
beam = Image.new('RGBA', (S, S), (0, 0, 0, 0))
bd = ImageDraw.Draw(beam)
for w, a in ((90, 26), (46, 60), (20, 130), (8, 255)):
    bd.line([SWORD_TIP, SIGIL], fill=(255, 248, 220, a), width=w)
for r, a in ((70, 50), (40, 120), (18, 255)):
    bd.ellipse([SIGIL[0] - r, SIGIL[1] - r, SIGIL[0] + r, SIGIL[1] + r], fill=(200, 230, 255, a))
art = Image.alpha_composite(art, beam.filter(ImageFilter.GaussianBlur(10)))
d = ImageDraw.Draw(art)
d.line([SWORD_TIP, SIGIL], fill=(255, 253, 240, 255), width=4)
# the sigil itself: a small glowing rune
sx, sy = SIGIL
d.line([(sx, sy - 16), (sx, sy + 16)], fill=(235, 248, 255), width=5)
d.line([(sx - 12, sy - 6), (sx + 12, sy - 6)], fill=(235, 248, 255), width=4)
d.arc([sx - 12, sy - 4, sx + 12, sy + 20], 0, 180, fill=(235, 248, 255), width=4)

# --- Rounded icon frame ---
mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([16, 16, S - 16, S - 16], radius=190, fill=255)
icon = Image.new('RGBA', (S, S), (0, 0, 0, 0))
icon.paste(art, (0, 0), mask)
ImageDraw.Draw(icon).rounded_rectangle([16, 16, S - 16, S - 16], radius=190,
                                       outline=(196, 164, 98, 255), width=22)

out = sys.argv[1] if len(sys.argv) > 1 else 'sotc'
icon.resize((256, 256), Image.LANCZOS).save(out + '.png')
icon.save(out + '.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print('wrote', out + '.png', out + '.ico')
