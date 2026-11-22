"""Card renderer: turns a card into one 800 x 480 1-bit frame, the answer view, plus the rows the device blanks to
make the question view. The device never lays out card text, it only shows these frames.

Frame bytes: 100 bytes per row, 480 rows, most significant bit first, 1 = black. Sent zlib-compressed.
"""
import os
import zlib

from PIL import Image, ImageDraw, ImageFont

from . import fsrs

W, H = 800, 480
STRIP_Y = 432                       # key label strip: rows 432..479
KEY_X = (100, 300, 500, 700)        # screen x right above each key (keys sit at -61.2, -20.4, 20.4, 61.2 mm)
MARGIN_X, TOP, BOTTOM = 48, 40, STRIP_Y - 16
FONT_DIR = os.path.join(os.path.dirname(__file__), "fonts")
_cache = {}


def font(size, bold=False):
    key = (size, bold)
    if key not in _cache:
        _cache[key] = ImageFont.truetype(os.path.join(FONT_DIR, "Inter-SemiBold.ttf" if bold else "Inter-Regular.ttf"), size)
    return _cache[key]


def wrap(text, f, width):
    lines = []
    for para in text.split("\n"):
        words, line = para.split(" "), ""
        for w in words:
            t = (line + " " + w).strip()
            if f.getlength(t) <= width or not line:
                line = t
            else:
                lines.append(line)
                line = w
        lines.append(line)
    return lines


def _draw_lines(d, lines, f, x, y, lh, center_w=None):
    for ln in lines:
        if not ln:                          # a blank line between paragraphs: half a line of space
            y += lh // 2
            continue
        if center_w:
            d.text((x + (center_w - f.getlength(ln)) / 2, y), ln, font=f, fill=0)
        else:
            d.text((x, y), ln, font=f, fill=0)
        y += lh
    return y


def _height(lines, lh):
    """How far _draw_lines moves down for these lines."""
    return sum(lh if ln else lh // 2 for ln in lines)


def _to_1bit(img):
    return img.point(lambda v: 0 if v < 150 else 255, mode="1")


def pack(img1):
    """1-bit PIL image -> device bytes (1 = black)"""
    return bytes(b ^ 0xFF for b in img1.convert("1").tobytes())


def unpack(data):
    return Image.frombytes("1", (W, H), bytes(b ^ 0xFF for b in data))


# ------------------------------------------------------------------ strips under each view
def strip(kind, labels=None):
    img = Image.new("L", (W, H - STRIP_Y), 255)
    d = ImageDraw.Draw(img)
    if kind == "grades":
        for x, (name, ivl) in zip(KEY_X, labels):
            f1, f2 = font(17, True), font(15)
            d.text((x - f1.getlength(name) / 2, 4), name, font=f1, fill=0)
            d.text((x - f2.getlength(ivl) / 2, 26), ivl, font=f2, fill=0)
    else:
        text = {"question": "any key shows the answer", "choice": "press the key under your answer",
                "next": "any key for the next card"}[kind]
        f = font(15)
        d.text(((W - f.getlength(text)) / 2, 16), text, font=f, fill=0)
    return _to_1bit(img)


def asset(name):
    """Shared strips the device pastes over a question view (rows 432..479)."""
    full = Image.new("1", (W, H), 1)
    full.paste(strip(name), (0, STRIP_Y))
    return full


# ------------------------------------------------------------------ cards
def render_basic(card, intervals):
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)
    width = W - 2 * MARGIN_X
    img_h = card.get("image_h", 0) if card.get("image") else 0
    avail = BOTTOM - TOP - (img_h + 18 if img_h else 0)
    for size in range(34, 17, -1):
        fq, fa = font(size, True), font(size)
        lh = round(size * 1.28)
        ql, al = wrap(card["front"], fq, width), wrap(card["back"], fa, width)
        need = _height(ql, lh) + 26 + _height(al, lh)
        if need <= avail:
            break
    else:
        max_a = max(1, (avail - _height(ql, lh) - 26) // lh)
        al = al[:max_a]
        al[-1] = al[-1].rstrip(".") + "..."
    y = _draw_lines(d, ql, fq, MARGIN_X, TOP, lh)
    if img_h:
        pic = card["image"](width, img_h)
        y += 10
        y_img = y
        y += img_h + 8
    ans_y = y + 26
    _draw_lines(d, al, fa, MARGIN_X, ans_y, lh)
    out = _to_1bit(img)
    if img_h:
        out.paste(pic, (MARGIN_X, y_img))
    out.paste(strip("grades", [(n, fsrs.label(ms)) for n, ms in zip(("Again", "Hard", "Good", "Easy"), intervals)]), (0, STRIP_Y))
    return out, dict(hide=[[ans_y - 14, STRIP_Y]], qstrip="question", choices=None)


def render_choice(card):
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)
    width = W - 2 * MARGIN_X
    box_w, box_y0, box_y1 = 184, 296, BOTTOM
    for size in range(30, 17, -1):
        fq = font(size, True)
        lh = round(size * 1.28)
        ql = wrap(card["front"], fq, width)
        q_end = TOP + _height(ql, lh)
        fe = font(max(15, size - 6))
        lhe = round((max(15, size - 6)) * 1.3)
        el = wrap(card["back"], fe, width)
        if q_end + 16 + _height(el, lhe) <= box_y0 - 12:
            break
    else:                                   # still too long at the smallest size: cut the explanation short
        max_e = max(1, (box_y0 - 12 - q_end - 16) // lhe)
        if len(el) > max_e:
            el = el[:max_e]
            el[-1] = el[-1].rstrip(".") + "..."
    _draw_lines(d, ql, fq, MARGIN_X, TOP, lh)
    expl_y = q_end + 16
    _draw_lines(d, el, fe, MARGIN_X, expl_y, lhe)
    for size in range(20, 12, -1):          # one size for all four options, so the boxes match
        fo = font(size)
        lho = round(size * 1.25)
        wrapped = [wrap(opt, fo, box_w - 20) for opt in card["options"]]
        if all(len(ol) * lho <= box_y1 - box_y0 - 16 and max(fo.getlength(ln) for ln in ol) <= box_w - 20
               for ol in wrapped):
            break
    boxes = []
    for x, ol in zip(KEY_X, wrapped):
        bx = x - box_w // 2
        d.rounded_rectangle([bx, box_y0, bx + box_w, box_y1], radius=10, outline=0, width=2)
        ty = box_y0 + (box_y1 - box_y0 - len(ol) * lho) / 2
        _draw_lines(d, ol, fo, bx + 10, ty, lho, center_w=box_w - 20)
        boxes.append([bx, box_y0, box_w, box_y1 - box_y0])
    out = _to_1bit(img)
    out.paste(strip("next"), (0, STRIP_Y))
    return out, dict(hide=[[expl_y - 6, box_y0 - 4]], qstrip="choice", choices=dict(boxes=boxes, correct=card["correct"]))


def render(card, intervals):
    """Returns (zlib bytes, crc32 of the raw frame, meta dict)."""
    img, meta = render_choice(card) if card["kind"] == "choice" else render_basic(card, intervals)
    raw = pack(img)
    return zlib.compress(raw, 9), zlib.crc32(raw), meta


def question_view(img, meta):
    """What the device shows before a reveal, for previews on the dashboard."""
    q = img.copy()
    d = ImageDraw.Draw(q)
    for y0, y1 in meta["hide"]:
        d.rectangle([0, y0, W - 1, y1 - 1], fill=1)
    if meta["qstrip"]:
        q.paste(strip(meta["qstrip"]), (0, STRIP_Y))
    return q
