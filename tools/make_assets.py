import math, random
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops

S = 2
W, H = 900 * S, 720 * S
FD = "/usr/share/fonts/truetype/"
import os
HERE = os.path.dirname(os.path.abspath(__file__))
def fp(name):
    return os.path.join(HERE, "assets", name)
INTER_BLK_IT = fp("Inter-BlackItalic.otf")
INTER_XBI = fp("Inter-ExtraBoldItalic.otf")
INTER_BD = fp("DejaVuSansMono-Bold.ttf")
MONO = fp("DejaVuSansMono.ttf")
MONOB = fp("DejaVuSansMono-Bold.ttf")

def F(path, size): return ImageFont.truetype(path, int(size * S))
def sc(v): return int(v * S)

def vgrad(w, h, stops):
    """stops: list of (t, (r,g,b))"""
    g = Image.new("RGB", (1, h))
    px = g.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        for i in range(len(stops) - 1):
            t0, c0 = stops[i]; t1, c1 = stops[i + 1]
            if t0 <= t <= t1:
                u = (t - t0) / max(t1 - t0, 1e-9)
                px[0, y] = tuple(int(c0[k] + (c1[k] - c0[k]) * u) for k in range(3))
                break
    return g.resize((w, h))

def glow(base, drawfn, color, radius, strength=1.0):
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    drawfn(ImageDraw.Draw(layer), color)
    b = layer.filter(ImageFilter.GaussianBlur(radius * S))
    if strength != 1.0:
        a = b.getchannel("A").point(lambda v: min(255, int(v * strength)))
        b.putalpha(a)
    base.alpha_composite(b)

def text_mask(size, xy, text, font, anchor="mm"):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).text(xy, text, font=font, fill=255, anchor=anchor)
    return m

def paste_grad_text(img, xy, text, font, stops, anchor="mm", outline=None, shadow=None):
    m = text_mask(img.size, xy, text, font, anchor)
    bbox = m.getbbox()
    if not bbox: return
    x0, y0, x1, y1 = bbox
    g = vgrad(img.size[0], y1 - y0, stops)
    full = Image.new("RGB", img.size)
    full.paste(g, (0, y0))
    if shadow:
        sm = ImageChops.offset(m, *shadow[0])
        img.paste(Image.new("RGBA", img.size, shadow[1]), (0, 0), sm)
    if outline:
        om = m.filter(ImageFilter.MaxFilter(2 * outline[0] + 1))
        img.paste(Image.new("RGBA", img.size, outline[1]), (0, 0), om)
    img.paste(full.convert("RGBA"), (0, 0), m)

def pill(w, h, top, bot, border, gloss=150, rim=None):
    K = 3
    ww, hh = w * K, h * K
    outer = Image.new("L", (ww, hh), 0)
    ImageDraw.Draw(outer).rounded_rectangle([0, 0, ww - 1, hh - 1], radius=hh // 2, fill=255)
    b = 2 * K
    inner = Image.new("L", (ww, hh), 0)
    ImageDraw.Draw(inner).rounded_rectangle([b, b, ww - 1 - b, hh - 1 - b], radius=(hh - 2 * b) // 2, fill=255)
    if rim:
        img = vgrad(ww, hh, rim)
    else:
        img = Image.new("RGB", (ww, hh), border)
    img.paste(vgrad(ww, hh, [(0, top), (1, bot)]), (0, 0), inner)
    gh = int(hh * 0.5); ins = 4 * K
    shape = Image.new("L", (ww, hh), 0)
    ImageDraw.Draw(shape).rounded_rectangle([ins, b + K, ww - 1 - ins, gh], radius=(gh - b) // 2, fill=255)
    fade = Image.new("L", (ww, hh), 0)
    fd = ImageDraw.Draw(fade)
    for y in range(gh):
        fd.line([(0, y), (ww, y)], fill=int(gloss * (1 - y / gh)))
    img.paste(Image.new("RGB", (ww, hh), (230, 255, 238)), (0, 0), ImageChops.multiply(shape, fade))
    o = img.convert("RGBA"); o.putalpha(outer)
    return o.resize((w, h), Image.LANCZOS)


# ------------------------------------------------------------------ RP2040-Zero chip icon
def chip_rp2040(img, cx, cy):
    """Little RP2040-Zero board: USB-C on the left, castellated edges, QFN, buttons, LED."""
    bw, bh = 80, 58
    L, T_, R, B = cx - bw // 2, cy - bh // 2, cx + bw // 2, cy + bh // 2
    neon = (57, 255, 20)
    def g(dd, c):
        dd.rounded_rectangle([sc(L), sc(T_), sc(R), sc(B)], radius=sc(6), outline=c, width=sc(3))
    glow(img, g, neon + (255,), 6, 1.3)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([sc(L), sc(T_), sc(R), sc(B)], radius=sc(6), fill=(10, 32, 24), outline=neon, width=sc(2))
    # castellated pads top + bottom
    n = 10
    for i in range(n):
        px = L + 9 + i * (bw - 18) / (n - 1)
        for py in (T_, B):
            d.rounded_rectangle([sc(px - 2), sc(py - 3), sc(px + 2), sc(py + 3)], radius=sc(1.5), fill=(232, 196, 84), outline=(150, 120, 40))
            d.ellipse([sc(px - 1), sc(py - 1), sc(px + 1), sc(py + 1)], fill=(10, 32, 24))
    # USB-C connector (silver) sticking out the left
    ux0, ux1 = L - 9, L + 15
    ug = vgrad(sc(ux1 - ux0), sc(22), [(0, (240, 243, 248)), (0.5, (170, 176, 186)), (1, (215, 220, 228))])
    um = Image.new("L", ug.size, 0)
    ImageDraw.Draw(um).rounded_rectangle([0, 0, ug.size[0] - 1, ug.size[1] - 1], radius=sc(4), fill=255)
    img.paste(ug.convert("RGBA"), (sc(ux0), sc(cy - 11)), um)
    d.rounded_rectangle([sc(ux0), sc(cy - 11), sc(ux1), sc(cy + 11)], radius=sc(4), outline=(90, 96, 106), width=sc(1))
    # BOOT / RESET buttons
    for by in (cy - 18, cy + 18):
        d.rounded_rectangle([sc(L + 18), sc(by - 3), sc(L + 26), sc(by + 3)], radius=sc(1), fill=(200, 204, 210), outline=(80, 84, 92))
        d.ellipse([sc(L + 21), sc(by - 1), sc(L + 23), sc(by + 1)], fill=(60, 64, 70))
    # RP2040 QFN
    qx, qy, qs = cx + 9, cy, 24
    for k in range(7):
        o = -qs / 2 + 3 + k * (qs - 6) / 6
        d.line([sc(qx + o), sc(qy - qs / 2 - 2.5), sc(qx + o), sc(qy - qs / 2)], fill=(190, 194, 200), width=sc(1))
        d.line([sc(qx + o), sc(qy + qs / 2), sc(qx + o), sc(qy + qs / 2 + 2.5)], fill=(190, 194, 200), width=sc(1))
        d.line([sc(qx - qs / 2 - 2.5), sc(qy + o), sc(qx - qs / 2), sc(qy + o)], fill=(190, 194, 200), width=sc(1))
        d.line([sc(qx + qs / 2), sc(qy + o), sc(qx + qs / 2 + 2.5), sc(qy + o)], fill=(190, 194, 200), width=sc(1))
    d.rounded_rectangle([sc(qx - qs / 2), sc(qy - qs / 2), sc(qx + qs / 2), sc(qy + qs / 2)], radius=sc(2), fill=(16, 16, 20), outline=(88, 92, 100), width=sc(1))
    d.ellipse([sc(qx - qs / 2 + 3), sc(qy - qs / 2 + 3), sc(qx - qs / 2 + 6), sc(qy - qs / 2 + 6)], fill=(210, 214, 220))
    d.text((sc(qx + 1), sc(qy + 2)), "RP2040", font=F(MONOB, 3.6), fill=(150, 156, 166), anchor="mm")
    # flash chip
    d.rounded_rectangle([sc(R - 16), sc(cy - 20), sc(R - 6), sc(cy - 13)], radius=sc(1), fill=(20, 20, 24), outline=(80, 84, 92))

def logo(img):
    chip_w = 88
    f = F(INTER_BLK_IT, 56)
    tw = f.getlength("MATRIXPICO") / S
    gap = 16
    total = chip_w + gap + tw
    x0 = (900 - total) / 2
    chip_rp2040(img, int(x0 + 9 + 40), 54)
    tx = sc(x0 + chip_w + gap)
    cy = sc(54)
    wm = f.getlength("MATRIX")
    def g(dd, c): dd.text((tx, cy), "MATRIX", font=f, fill=c, anchor="lm")
    glow(img, g, (30, 255, 110, 255), 11, 1.3)
    def g2(dd, c): dd.text((tx + wm, cy), "PICO", font=f, fill=c, anchor="lm")
    glow(img, g2, (190, 205, 220, 255), 9, 0.9)
    paste_grad_text(img, (tx, cy), "MATRIX", f, [(0, (200, 255, 220)), (0.45, (40, 240, 120)), (1, (0, 130, 55))],
                    anchor="lm", shadow=((sc(2), sc(3)), (0, 0, 0, 210)))
    paste_grad_text(img, (tx + wm, cy), "PICO", f, [(0, (250, 252, 255)), (0.45, (178, 186, 196)), (0.55, (120, 128, 138)), (1, (205, 212, 222))],
                    anchor="lm", shadow=((sc(2), sc(3)), (0, 0, 0, 210)))

# ------------------------------------------------------------------ C2 background (cyan grid)
def bg_cyan():
    horizon = (0, 200, 255)
    img = vgrad(W, H, [(0, (2, 8, 24)), (0.55, (4, 22, 52)), (1, (2, 6, 16))]).convert("RGBA")
    d = ImageDraw.Draw(img)
    hz = int(H * 0.52)
    random.seed(4)
    for _ in range(90):
        x, y = random.randint(0, W), random.randint(0, int(H * 0.45))
        d.ellipse([x, y, x + sc(1.5), y + sc(1.5)], fill=(200, 200, 255, random.randint(60, 200)))
    lay = Image.new("RGBA", (W, H), (0, 0, 0, 0)); ld = ImageDraw.Draw(lay)
    col = horizon + (150,)
    for i in range(-14, 15):
        ld.line([(W // 2 + i * sc(18), hz), (W // 2 + i * sc(120), H)], fill=col, width=sc(1.2))
    for k in range(1, 14):
        t = (k / 14) ** 2
        y = hz + int((H - hz) * t)
        ld.line([(0, y), (W, y)], fill=col, width=sc(1.2))
    fadem = Image.new("L", (W, H), 0); fd = ImageDraw.Draw(fadem)
    for y in range(hz, H):
        fd.line([(0, y), (W, y)], fill=int(255 * min(1, (y - hz) / (H - hz) * 1.6)))
    lay.putalpha(ImageChops.multiply(lay.getchannel("A"), fadem))
    img.alpha_composite(lay)
    return img

PANEL = (2, 10, 28, 210); HEAD = (4, 24, 56, 255); BORDER = (0, 170, 230)
ACCENT = (0, 240, 255); TEXT = (210, 240, 255); MUTED = (110, 150, 190); TERM_BG = (2, 6, 18)

def panel(img, x, y, w, h, title=""):
    ov = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(ov).rectangle([sc(x), sc(y), sc(x + w), sc(y + h)], fill=PANEL)
    img.alpha_composite(ov)
    dd = ImageDraw.Draw(img)
    if title:
        dd.rectangle([sc(x), sc(y), sc(x + w), sc(y + 30)], fill=HEAD)
        dd.text((sc(x + 12), sc(y + 15)), title, font=F(MONOB, 12), fill=ACCENT, anchor="lm")
    dd.rectangle([sc(x), sc(y), sc(x + w), sc(y + h)], outline=BORDER, width=sc(1.2))

def build():
    img = bg_cyan()
    logo(img)
    panel(img, 20, 108, 860, 44)
    d = ImageDraw.Draw(img)
    d.text((sc(34), sc(130)), "PORT", font=F(MONOB, 10), fill=TEXT, anchor="lm")
    panel(img, 20, 166, 250, 538, "// CONSOLE")
    d = ImageDraw.Draw(img)
    d.text((sc(36), sc(216)), "select a board:", font=F(MONO, 9), fill=MUTED, anchor="lm")
    d.line([sc(36), sc(522), sc(254), sc(522)], fill=BORDER, width=sc(1))
    d.text((sc(36), sc(568)), "(save after tuning)", font=F(MONO, 8), fill=MUTED, anchor="lm")
    panel(img, 284, 166, 596, 538, "// DEBUG")
    d = ImageDraw.Draw(img)
    d.rectangle([sc(298), sc(248), sc(866), sc(660)], fill=TERM_BG, outline=BORDER, width=sc(1))
    d.text((sc(298), sc(682)), ">", font=F(MONOB, 13), fill=ACCENT, anchor="lm")
    d.rectangle([sc(312), sc(670), sc(780), sc(694)], fill=(2, 6, 26), outline=BORDER, width=sc(1))
    return img.convert("RGB").resize((900, 720), Image.LANCZOS)

if __name__ == "__main__":
    out = os.path.join(HERE, "assets", "bg.png")
    build().save(out)
    print("wrote", out)
