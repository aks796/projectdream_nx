#!/usr/bin/env python3
"""switch_glyphs.py <sdt.apk> <out dir>: the game's controller icons with the Switch's buttons, made from its own.

The game has three sets of button icons (Xbox, DualShock 4, DualSense) and none of the Switch's; with Project Dream's
GameController bridge it shows its default set (Xbox: LB, RB, the Menu and View buttons). Here those four icons are
remade from their own pixels -- the shapes and the letters cut out of them are the game's:
  LB -> L, RB -> R   the B's cut-out filled, the other letter centred
  Menu (three bars) -> +   its middle bar, and the same bar turned upright
  View (two windows) -> -  its circle, with the Menu icon's middle bar cut out
and each bundle is written again (the texture as RGBA32 instead of BC7) to <out dir>/<the bundle's file name>.
pd_shim.c's bundle_path loads these in place of the APK's (the catalog checks no CRC: every m_Crc is 0).

The bundles are made from the user's own APK, as sdt.apk is: game-derived, never published.
Needs UnityPy and Pillow (tools/switch_glyphs.sh makes a venv for them).
"""
import os
import sys
import zipfile

import UnityPy
from PIL import Image

ICONS = "assets/aa/StandaloneOSX/duplicateassetisolation_assets_assets/data/ui/sprites/nonatlassed/controllericons/"


def parts(im):
    """alpha of the solid shape (cut-outs filled), the cut-outs' own coverage (0..1) and labels of them"""
    w, h = im.size
    a = [im.getpixel((x, y))[3] for y in range(h) for x in range(w)]
    amax = max(a)  # (the BC7 icons top out under 255)
    # the outside: transparent pixels reached from the border
    out = [False] * (w * h)
    stack = [i for i in range(w * h) if (i % w in (0, w - 1) or i // w in (0, h - 1)) and a[i] < 128]
    for i in stack:
        out[i] = True
    while stack:
        i = stack.pop()
        x, y = i % w, i // w
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h:
                j = ny * w + nx
                if not out[j] and a[j] < 128:
                    out[j] = True
                    stack.append(j)
    # inside the shape: the cut-outs (alpha under full there), grown one pixel to take their soft edges
    hole = [0.0] * (w * h)
    for i in range(w * h):
        if not out[i] and a[i] < amax - 6:
            hole[i] = 1.0 - a[i] / float(amax)
    # the shape: one even alpha inside (BC7 left it uneven by a step or two), its outer edge as it was
    def edge(i):
        x, y = i % w, i // w
        return any(0 <= x + dx < w and 0 <= y + dy < h and out[(y + dy) * w + x + dx]
                   for dx in (-1, 0, 1) for dy in (-1, 0, 1))
    shape = [a[i] if out[i] or (edge(i) and hole[i] == 0) else amax for i in range(w * h)]
    # the cut-outs' pieces (4-connected over coverage > 0), left to right
    label = [0] * (w * h)
    n = 0
    boxes = []
    for i in range(w * h):
        if hole[i] > 0.02 and not label[i]:
            n += 1
            label[i] = n
            st = [i]
            xs = []
            while st:
                k = st.pop()
                xs.append(k)
                x, y = k % w, k // w
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h:
                        j = ny * w + nx
                        if hole[j] > 0.02 and not label[j]:
                            label[j] = n
                            st.append(j)
            boxes.append((min(k % w for k in xs), max(k % w for k in xs), min(k // w for k in xs),
                          max(k // w for k in xs), n, len(xs)))
    return shape, hole, label, boxes


def fill_colour(im, shape):
    w, h = im.size
    top = max(shape)
    px = [im.getpixel((i % w, i // w)) for i in range(w * h) if im.getpixel((i % w, i // w))[3] >= top - 4]
    px.sort(key=lambda p: p[0] + p[1] + p[2])
    return px[len(px) // 2][:3]


def compose(size, shape, cut, rgb):
    w, h = size
    out = Image.new("RGBA", size)
    for i in range(w * h):
        al = int(round(shape[i] * (1.0 - min(1.0, cut[i]))))
        out.putpixel((i % w, i // w), rgb + (al,))
    return out


def letter_alone(im, keep_first):
    """LB/RB: the first (or second) letter's cut-out alone, centred on the shape"""
    w, h = im.size
    shape, hole, label, boxes = parts(im)
    boxes = [b for b in boxes if b[5] > 8]
    boxes.sort(key=lambda b: b[0])
    letter = boxes[0] if keep_first else boxes[-1]
    sx = [i % w for i in range(w * h) if shape[i] > 128]
    centre = (min(sx) + max(sx)) / 2.0
    dx = int(round(centre - (letter[0] + letter[1]) / 2.0))
    cut = [0.0] * (w * h)
    for i in range(w * h):
        if label[i] == letter[4] or (hole[i] > 0 and label[i] == 0 and letter[0] - 1 <= i % w <= letter[1] + 1):
            x, y = i % w + dx, i // w
            if 0 <= x < w:
                cut[y * w + x] = max(cut[y * w + x], hole[i])
    return compose(im.size, shape, cut, fill_colour(im, shape))


def middle_bar(im):
    """the Menu icon: its middle bar's cut-out (coverage per pixel)"""
    w, h = im.size
    shape, hole, label, boxes = parts(im)
    sy = [i // w for i in range(w * h) if shape[i] > 128]
    cy = (min(sy) + max(sy)) / 2.0
    # the bar nearest the circle's middle (the cut-out's pieces include stray soft-edge bits: the big ones only)
    bars = [b for b in boxes if b[5] > 30]
    mid = min(bars, key=lambda b: abs((b[2] + b[3]) / 2.0 - cy))
    return [hole[i] if label[i] == mid[4] or (label[i] == 0 and hole[i] > 0 and mid[2] - 1 <= i // w <= mid[3] + 1
                                               and mid[0] - 1 <= i % w <= mid[1] + 1) else 0.0 for i in range(w * h)]


def plus_from_menu(im):
    w, h = im.size
    shape, _, _, _ = parts(im)
    bar = middle_bar(im)
    # the bar turned upright about its own middle
    xs = [i % w for i in range(w * h) if bar[i] > 0.5]
    ys = [i // w for i in range(w * h) if bar[i] > 0.5]
    cx, cy = (min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0
    # (an exact quarter turn of the bar's own pixels, centred where it was)
    x0, x1, y0, y1 = max(0, min(xs) - 2), min(w, max(xs) + 3), max(0, min(ys) - 2), min(h, max(ys) + 3)
    piece = Image.new("L", (x1 - x0, y1 - y0))
    for y in range(y0, y1):
        for x in range(x0, x1):
            piece.putpixel((x - x0, y - y0), int(round(bar[y * w + x] * 255)))
    up = piece.transpose(Image.ROTATE_90)
    ox, oy = int(round(cx - (up.width - 1) / 2.0)), int(round(cy - (up.height - 1) / 2.0))
    cut = list(bar)
    for y in range(up.height):
        for x in range(up.width):
            tx, ty = ox + x, oy + y
            if 0 <= tx < w and 0 <= ty < h:
                cut[ty * w + tx] = max(cut[ty * w + tx], up.getpixel((x, y)) / 255.0)
    return compose(im.size, shape, cut, fill_colour(im, shape))


def minus(view, menu):
    w, h = view.size
    shape, _, _, _ = parts(view)
    bar = middle_bar(menu) if menu.size == view.size else [0.0] * (w * h)
    return compose(view.size, shape, bar, fill_colour(view, shape))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    apk, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    z = zipfile.ZipFile(apk)
    names = {n.split("/")[-1].split(".png")[0]: n for n in z.namelist() if n.startswith(ICONS)}

    def load(key):
        env = UnityPy.load(z.read(names[key]))
        tex = next(o for o in env.objects if o.type.name == "Texture2D")
        return env, tex

    imgs = {}
    for key in ("controller_default_lb", "controller_default_rb", "controller_default_start",
                "controller_default_settings"):
        env, tex = load(key)
        imgs[key] = tex.read().image.convert("RGBA")
    made = {
        "controller_default_lb": letter_alone(imgs["controller_default_lb"], True),
        "controller_default_rb": letter_alone(imgs["controller_default_rb"], True),
        "controller_default_start": plus_from_menu(imgs["controller_default_start"]),
        "controller_default_settings": minus(imgs["controller_default_settings"], imgs["controller_default_start"]),
    }
    for key, im in made.items():
        env, tex = load(key)
        d = tex.read()
        d.set_image(im, target_format=4)  # RGBA32
        d.m_MipCount = 1
        d.save()
        path = os.path.join(out, names[key].split("/")[-1])
        with open(path, "wb") as f:
            f.write(env.file.save(packer="original"))
        im.save(os.path.join(out, key + ".png"))  # (to look at)
        print("%s: %dx%d -> %s" % (key, im.size[0], im.size[1], path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
