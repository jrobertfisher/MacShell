#!/usr/bin/env python3
"""Draws MacShell's icons (an original design: a dark rounded screen with a
   white ">" prompt and a grey "_" cursor) and writes icons.r:

     APP_ICON   ICN# icl4 icl8 ics# ics4 ics8   the application
     FILE_ICON  ICN# icl4 icl8 ics# ics4 ics8   MacShell's documents (preferences)
     PICT_ABOUT 128x128 black-and-white picture for the About box

   Usage: make_icons.py <output icons.r> <output preview.png>
"""
import sys
from PIL import Image, ImageDraw

SS = 16                                    # supersampling factor

SCREEN = (28, 28, 30)
BEZEL = (96, 96, 100)
PROMPT = (255, 255, 255)
CURSOR = (153, 153, 153)


def mac_palette8():
    """The standard Macintosh 8-bit colour table (clut 8)."""
    pal = []
    for i in range(215):
        pal.append((255 - 51 * (i // 36), 255 - 51 * ((i // 6) % 6), 255 - 51 * (i % 6)))
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    pal += [(v, 0, 0) for v in ramp] + [(0, v, 0) for v in ramp] + [(0, 0, v) for v in ramp]
    pal += [(v, v, v) for v in ramp]
    pal.append((0, 0, 0))
    assert len(pal) == 256
    return pal


PAL8 = mac_palette8()
PAL4 = [(255, 255, 255), (252, 243, 5), (255, 100, 2), (221, 8, 6), (242, 8, 132), (70, 0, 165),
        (0, 0, 212), (2, 171, 234), (31, 183, 20), (0, 100, 17), (86, 44, 5), (144, 113, 58),
        (192, 192, 192), (128, 128, 128), (64, 64, 64), (0, 0, 0)]


def nearest(rgb, pal):
    r, g, b = rgb
    gray = abs(r - g) < 12 and abs(g - b) < 12
    best, bi = None, 0
    for i, (pr, pg, pb) in enumerate(pal):
        if gray and not (pr == pg == pb):  # keep greys grey
            continue
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if best is None or d < best:
            best, bi = d, i
    return bi


def screen(d, box, size, small):
    """The rounded dark screen with the prompt, inside box (pixel units)."""
    x0, y0, x1, y1 = box
    w = x1 - x0
    s = SS
    rad = (0.26 if not small else 0.22) * w * s
    d.rounded_rectangle([x0 * s, y0 * s, x1 * s - 1, y1 * s - 1], radius=rad, fill=BEZEL)
    inset = (1.0 if w >= 20 else 0.75) * s
    d.rounded_rectangle([x0 * s + inset, y0 * s + inset, x1 * s - 1 - inset, y1 * s - 1 - inset],
                        radius=max(rad - inset, 1), fill=SCREEN)
    # ">" in the upper left, "_" to its lower right
    lw = max(int(w * s * (0.085 if not small else 0.12)), s)
    px, py = x0 + w * 0.24, y0 + w * 0.26
    pw, ph = w * 0.2, w * 0.36
    pts = [(px * s, py * s), ((px + pw) * s, (py + ph / 2) * s), (px * s, (py + ph) * s)]
    d.line(pts, fill=PROMPT, width=lw, joint='curve')
    cx0, cx1 = x0 + w * 0.50, x0 + w * 0.76
    cy = y0 + w * 0.68
    d.rectangle([cx0 * s, cy * s, cx1 * s, cy * s + lw - 1], fill=CURSOR)


def app_icon(size):
    img = Image.new('RGBA', (size * SS, size * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    m = 1 if size == 16 else 2
    screen(d, (m, m, size - m, size - m), size, size == 16)
    return img.resize((size, size), Image.LANCZOS)


def doc_icon(size):
    """A page with a folded corner and a small screen on it."""
    img = Image.new('RGBA', (size * SS, size * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = SS
    if size == 32:
        l, t, r, b, fold = 4, 1, 27, 31, 7
    else:
        l, t, r, b, fold = 2, 0, 14, 16, 4
    page = [(l * s, t * s), ((r - fold) * s, t * s), (r * s - 1, (t + fold) * s),
            (r * s - 1, b * s - 1), (l * s, b * s - 1)]
    d.polygon(page, fill=(0, 0, 0))
    inner = [((l + 1) * s, (t + 1) * s), ((r - fold - 0.5) * s, (t + 1) * s),
             ((r - 1) * s - 1, (t + fold + 0.5) * s), ((r - 1) * s - 1, (b - 1) * s - 1),
             ((l + 1) * s, (b - 1) * s - 1)]
    d.polygon(inner, fill=(255, 255, 255))
    d.polygon([((r - fold) * s, t * s), ((r - fold) * s, (t + fold) * s - 1),
               (r * s - 1, (t + fold) * s - 1)], fill=(0, 0, 0))
    if size == 32:
        screen(d, (8, 12, 24, 28), 16, True)
    else:
        screen(d, (4, 7, 12, 15), 8, True)
    return img.resize((size, size), Image.LANCZOS)


def flat(px):
    r, g, b, a = px
    a /= 255
    return tuple(round(v * a + 255 * (1 - a)) for v in (r, g, b))


def to_bw(img):
    w, h = img.size
    icon = [[0] * w for _ in range(h)]
    mask = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b, a = img.getpixel((x, y))
            mask[y][x] = 1 if a >= 100 else 0
            lum = 0.3 * r + 0.59 * g + 0.11 * b
            icon[y][x] = 1 if mask[y][x] and lum < 140 else 0
    return icon, mask


def to_index(img, pal):
    w, h = img.size
    return [[nearest(flat(img.getpixel((x, y))), pal) if img.getpixel((x, y))[3] >= 100 else 0
             for x in range(w)] for y in range(h)]


def bits_hex(rows):
    out = []
    for row in rows:
        v = 0
        for b in row:
            v = (v << 1) | b
        out.append(f'{v:0{len(row) // 4}X}')
    return out


def rez(rtype, rid, hexdata):
    """hexdata: one long hex string."""
    words = [hexdata[i:i + 4] for i in range(0, len(hexdata), 4)]
    lines = [' '.join(words[i:i + 8]) for i in range(0, len(words), 8)]
    body = '\n'.join(f'\t$"{l}"' for l in lines)
    return f"data '{rtype}' ({rid}, purgeable) {{\n{body}\n}};\n\n"


def icon_family(rid, big, small, out):
    icn, icn_m = to_bw(big)
    ics, ics_m = to_bw(small)
    out.append(rez('ICN#', rid, ''.join(bits_hex(icn + icn_m))))
    out.append(rez('ics#', rid, ''.join(bits_hex(ics + ics_m))))
    for rtype, img, pal, fmt in (('icl4', big, PAL4, '{:X}'), ('ics4', small, PAL4, '{:X}'),
                                 ('icl8', big, PAL8, '{:02X}'), ('ics8', small, PAL8, '{:02X}')):
        idx = to_index(img, pal)
        out.append(rez(rtype, rid, ''.join(fmt.format(v) for row in idx for v in row)))
    return icn_m, ics_m


def about_pict():
    """128x128 one-bit PICT (version 1, BitsRect) of the app icon."""
    size = 128
    img = Image.new('RGBA', (size * 4, size * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    global SS
    old, SS = SS, 4
    screen(d, (4, 4, size - 4, size - 4), size, False)
    SS = old
    img = img.resize((size, size), Image.LANCZOS)
    icon, _ = to_bw(img)
    rows = bits_hex(icon)
    body = bytes.fromhex(''.join(rows))
    rect = (0).to_bytes(2, 'big') * 2 + size.to_bytes(2, 'big') * 2
    pic = bytearray()
    pic += rect                                   # picFrame
    pic += b'\x11\x01'                            # version 1
    pic += b'\x90'                                # BitsRect
    pic += (size // 8).to_bytes(2, 'big')         # rowBytes
    pic += rect + rect + rect                     # bounds, srcRect, dstRect
    pic += b'\x00\x00'                            # mode: srcCopy
    pic += body
    pic += b'\xff'                                # end of picture
    pic = (len(pic) + 2).to_bytes(2, 'big') + pic
    if len(pic) % 2:
        pic += b'\x00'
    return pic.hex().upper(), img


def main(out_r, out_png):
    app_big, app_small = app_icon(32), app_icon(16)
    doc_big, doc_small = doc_icon(32), doc_icon(16)
    out = ['/* MacShell icons: an original design, generated by tools/make_icons.py.\n'
           '   Edit the script, not this file. */\n\n']
    icon_family('APP_ICON', app_big, app_small, out)
    icon_family('FILE_ICON', doc_big, doc_small, out)
    pict_hex, pict_img = about_pict()
    out.append(rez('PICT', 'PICT_ABOUT', pict_hex))
    with open(out_r, 'w') as f:
        f.write(''.join(out))

    # preview: 8-bit, 4-bit and B&W at 8x on the classic grey desktop, plus the About picture
    scale, pad = 6, 16
    tiles = []
    for big, small in ((app_big, app_small), (doc_big, doc_small)):
        for kind in ('8', '4', 'bw'):
            for img in (big, small):
                n = img.size[0]
                if kind == 'bw':
                    icon, mask = to_bw(img)
                    pix = [[(0, 0, 0) if icon[y][x] else (255, 255, 255) for x in range(n)] for y in range(n)]
                else:
                    pal = PAL8 if kind == '8' else PAL4
                    idx = to_index(img, pal)
                    _, mask = to_bw(img)
                    pix = [[pal[idx[y][x]] for x in range(n)] for y in range(n)]
                t = Image.new('RGB', (n, n))
                m = Image.new('L', (n, n))
                for y in range(n):
                    for x in range(n):
                        t.putpixel((x, y), pix[y][x])
                        m.putpixel((x, y), 255 if mask[y][x] else 0)
                tiles.append((t.resize((n * scale,) * 2, Image.NEAREST), m.resize((n * scale,) * 2, Image.NEAREST)))
    W = pad + sum(t.size[0] + pad for t, _ in tiles[:6]) + 128 + pad
    H = pad + (32 * scale + pad) * 2
    prev = Image.new('RGB', (W, H), (170, 170, 204))
    for row in range(2):
        x = pad
        for t, m in tiles[row * 6:row * 6 + 6]:
            prev.paste(t, (x, pad + row * (32 * scale + pad)), m)
            x += t.size[0] + pad
    icon, _ = to_bw(pict_img)
    p = Image.new('RGB', (128, 128), (255, 255, 255))
    for y in range(128):
        for x in range(128):
            if icon[y][x]:
                p.putpixel((x, y), (0, 0, 0))
    prev.paste(p, (W - 128 - pad, pad))
    prev.save(out_png)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
