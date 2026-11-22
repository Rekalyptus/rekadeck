"""Mock Rekalyptus device API (FastAPI). Speaks the same protocol the real backend will, see PROTOCOL.md.

Run:  python -m rekaserver            (serves on 0.0.0.0:8000, dashboard at http://127.0.0.1:8000)

Environment: PORT, REKASIM_DB (the database file), REKASIM_APP_ONLY (how many due cards are picture cards only the
app can show, default 0; the dashboard can change it), REKASIM_FIRMWARE_DIR (releases to offer, see firmware.py).
"""
import io
import json
import os
import random
import secrets
import string
import time
import zlib

from fastapi import Body, FastAPI, Form, Header, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse, Response

from . import db, deck, firmware, fsrs, render
from .dashboard import DASHBOARD_HTML, LINK_HTML, LINKED_HTML

app = FastAPI(title="Rekalyptus device API (mock)")
FRAMES = {}          # (card_id, rev) -> (zlib bytes, crc32, meta)
ASSETS = {}          # name -> (zlib bytes, crc32)
SETTINGS = dict(idle_s=120, again_gap=5, clean_after_pct=15, checkin_every=10, wake_every_min=240, batch_limit=60)
CODE_TTL_MS = 10 * 60_000
# The mock's cards are all text cards, so picture cards (image occlusion and the like, which only the app can show)
# are pretended: this many of the due cards count as app-only, and the device's due_device leaves them out.
SIM = dict(app_only=max(0, int(os.environ.get("REKASIM_APP_ONLY", "0") or 0)))


def _device(auth):
    if not auth or not auth.startswith("Bearer "):
        raise HTTPException(401, "missing token")
    r = db.conn().execute("select * from devices where token=?", (auth[7:],)).fetchone()
    if not r:
        raise HTTPException(401, "unknown token")
    return r


def _frame(card_row):
    key = (card_row["id"], card_row["rev"])
    if key not in FRAMES:
        card = db.card_full(card_row)
        ivl = fsrs.preview(json.loads(card_row["state"]), db.now_ms())
        FRAMES[key] = render.render(card, ivl)
    return FRAMES[key]


def _asset(name):
    if name not in ASSETS:
        raw = render.pack(render.asset(name))
        ASSETS[name] = (zlib.compress(raw, 9), zlib.crc32(raw))
    return ASSETS[name]


def _map_time(device_id, boot, rtc):
    """Device clock (ms, drifting, since its last power-on) -> server time, using the (rtc, server) pairs from every
    check-in this boot. Interpolates between the two nearest pairs, which also cancels steady clock drift."""
    rows = db.conn().execute("select rtc_ms, server_ms from anchors where device_id=? and boot=? order by rtc_ms",
                             (device_id, boot)).fetchall()
    if not rows:
        return None
    pts = [(r["rtc_ms"], r["server_ms"]) for r in rows]
    before = [p for p in pts if p[0] <= rtc]
    after = [p for p in pts if p[0] >= rtc]
    if before and after:
        a, b = before[-1], after[0]
    elif len(pts) >= 2:
        a, b = (pts[0], pts[1]) if not before else (pts[-2], pts[-1])
    else:
        return pts[0][1] + (rtc - pts[0][0])
    if b[0] - a[0] < 60_000:
        return a[1] + (rtc - a[0])
    scale = (b[1] - a[1]) / (b[0] - a[0])
    return int(a[1] + (rtc - a[0]) * scale)


def _due_counts():
    t = db.now_ms()
    c = db.conn()
    due_now = c.execute("select count(*) from cards where due_ms<=?", (t,)).fetchone()[0]
    due_today = c.execute("select count(*) from cards where due_ms<=?", (t + 12 * 3600_000,)).fetchone()[0]
    return due_now, due_today


def _due_device(due_now):
    """How many of the due cards this device can show: due_now without the app-only ones, never more than due_now."""
    return max(0, due_now - SIM["app_only"])


# ------------------------------------------------------------------ linking a device (OAuth device-code style)
@app.post("/api/device/code")
def device_code(request: Request, body: dict = Body(default={})):
    dc = secrets.token_urlsafe(24)
    alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
    uc = "".join(random.choice(alphabet) for _ in range(3)) + "-" + "".join(random.choice(alphabet) for _ in range(3))
    c = db.conn()
    c.execute("insert into codes values (?,?,?,0,null)", (dc, uc, db.now_ms()))
    c.commit()
    base = str(request.base_url).rstrip("/")
    return dict(device_code=dc, user_code=uc, verify_url=f"{base}/link", interval_s=3, expires_s=CODE_TTL_MS // 1000)


@app.post("/api/device/token")
def device_token(body: dict = Body(...)):
    c = db.conn()
    r = c.execute("select * from codes where device_code=?", (body.get("device_code"),)).fetchone()
    if not r or db.now_ms() - r["created_ms"] > CODE_TTL_MS:
        return dict(status="expired")
    if not r["approved"]:
        return dict(status="pending")
    d = c.execute("select * from devices where id=?", (r["device_id"],)).fetchone()
    c.execute("delete from codes where device_code=?", (r["device_code"],))
    c.commit()
    return dict(status="ok", token=d["token"], device_id=d["id"])


@app.get("/link", response_class=HTMLResponse)
def link_page(code: str = ""):
    return LINK_HTML.replace("{{code}}", code)


@app.post("/link", response_class=HTMLResponse)
def link_submit(code: str = Form(...), name: str = Form("Rekadeck")):
    code = code.strip().upper().replace(" ", "")
    if len(code) == 6:
        code = code[:3] + "-" + code[3:]
    c = db.conn()
    r = c.execute("select * from codes where user_code=? and approved=0", (code,)).fetchone()
    if not r or db.now_ms() - r["created_ms"] > CODE_TTL_MS:
        return LINK_HTML.replace("{{code}}", code).replace("<!--err-->", '<p class="err">That code isn\'t valid or has expired.</p>')
    dev_id = "dev_" + secrets.token_hex(4)
    c.execute("insert into devices (id, token, name, created_ms) values (?,?,?,?)",
              (dev_id, secrets.token_urlsafe(24), name or "Rekadeck", db.now_ms()))
    c.execute("update codes set approved=1, device_id=? where device_code=?", (dev_id, r["device_code"]))
    c.commit()
    return LINKED_HTML.replace("{{name}}", name or "Rekadeck")


# ------------------------------------------------------------------ sync
@app.post("/api/device/hello")
def hello(body: dict = Body(...), authorization: str = Header(None)):
    d = _device(authorization)
    t = db.now_ms()
    c = db.conn()
    c.execute("insert into anchors values (?,?,?,?)", (d["id"], body.get("boot", 0), body.get("rtc_ms", 0), t))
    c.execute("update devices set fw=?, battery=?, charging=?, last_seen_ms=?, cards_left=?, unsynced=? where id=?",
              (body.get("fw"), body.get("battery"), int(bool(body.get("charging"))), t, body.get("cards_left"),
               body.get("unsynced"), d["id"]))
    c.commit()
    version = db.content_version()
    due_now, due_today = _due_counts()
    stale = (body.get("batch_id") != d["batch_id"] or body.get("batch_version") != version
             or (body.get("cards_left", 0) == 0 and due_now > 0))
    settings = dict(SETTINGS, utc_offset_min=time.localtime().tm_gmtoff // 60)   # this Mac's time zone stands in for the account's
    return dict(server_ms=t, version=version, stale=bool(stale), due_now=due_now, due_today=due_today,
                due_device=_due_device(due_now), settings=settings, firmware=firmware.offer(body.get("fw")))


@app.post("/api/device/unlink")
def unlink(authorization: str = Header(None)):
    d = _device(authorization)
    c = db.conn()
    c.execute("delete from anchors where device_id=?", (d["id"],))
    c.execute("delete from devices where id=?", (d["id"],))
    c.commit()
    return dict(status="ok")


@app.get("/api/device/batch")
def batch(limit: int = 60, authorization: str = Header(None)):
    d = _device(authorization)
    t = db.now_ms()
    rows = db.conn().execute("select * from cards where due_ms<=? order by due_ms limit ?",
                             (t + 24 * 3600_000, max(1, min(limit, 300)))).fetchall()
    cards = []
    for r in rows:
        z, crc, meta = _frame(r)
        cards.append(dict(id=r["id"], rev=r["rev"], kind=json.loads(r["body"])["kind"], crc=crc, size=len(z),
                          due_ms=r["due_ms"], **meta))
    version = db.content_version()
    batch_id = secrets.token_hex(4)
    c = db.conn()
    c.execute("update devices set batch_id=?, batch_version=? where id=?", (batch_id, version, d["id"]))
    c.commit()
    assets = [dict(name=n, crc=_asset(n)[1], size=len(_asset(n)[0])) for n in ("question", "choice")]
    return dict(batch_id=batch_id, version=version, issued_ms=t, assets=assets, cards=cards)


@app.get("/api/device/frame/{card_id}")
def frame(card_id: str, rev: int = -1, authorization: str = Header(None)):
    _device(authorization)
    r = db.conn().execute("select * from cards where id=?", (card_id,)).fetchone()
    if not r:
        raise HTTPException(404)
    if rev >= 0 and (card_id, rev) in FRAMES:
        z, crc, _ = FRAMES[(card_id, rev)]
    else:
        z, crc, _ = _frame(r)
    return Response(z, media_type="application/octet-stream", headers={"X-Crc32": str(crc)})


@app.get("/api/device/asset/{name}")
def asset(name: str, authorization: str = Header(None)):
    _device(authorization)
    if name not in ("question", "choice"):
        raise HTTPException(404)
    z, crc = _asset(name)
    return Response(z, media_type="application/octet-stream", headers={"X-Crc32": str(crc)})


@app.get("/api/device/firmware/{version}")
def firmware_image(version: str, authorization: str = Header(None)):
    """The app image for an over-the-air update. Content-Length comes with the body; X-Sha256 is a convenience, the
    device checks against the size and sha256 that hello promised."""
    _device(authorization)
    found = firmware.image(version)
    if not found:
        raise HTTPException(404)
    data, sha = found
    return Response(data, media_type="application/octet-stream", headers={"X-Sha256": sha})


@app.post("/api/device/reviews")
def reviews(body: dict = Body(...), authorization: str = Header(None)):
    d = _device(authorization)
    t = db.now_ms()
    c = db.conn()
    c.execute("insert into anchors values (?,?,?,?)", (d["id"], body.get("boot", 0), body.get("rtc_ms", 0), t))
    c.commit()
    accepted = []
    items = sorted(body.get("reviews", []), key=lambda r: r.get("graded", 0))
    for rv in items:
        boot = rv.get("boot", body.get("boot", 0))
        conv = lambda v: (_map_time(d["id"], boot, v) if v else None)
        graded = conv(rv.get("graded")) or t
        try:
            c.execute("insert into reviews (card_id, grade, source, device_id, rid, boot, shown_ms, revealed_ms, graded_ms,"
                      " choice, received_ms) values (?,?,?,?,?,?,?,?,?,?,?)",
                      (rv["card"], rv["g"], "rekadeck", d["id"], rv["rid"], boot, conv(rv.get("shown")),
                       conv(rv.get("revealed")), graded, rv.get("choice", -1), t))
            c.commit()
            if rv["g"] >= 1:
                db.apply_review(rv["card"], rv["g"], graded)
        except Exception as e:              # duplicate rid: already stored, still acknowledge it
            if "UNIQUE" not in str(e):
                raise
        accepted.append(rv["rid"])
    # clock drift seen across this boot's check-ins, for the dashboard
    rows = c.execute("select rtc_ms, server_ms from anchors where device_id=? and boot=? order by rtc_ms",
                     (d["id"], body.get("boot", 0))).fetchall()
    if len(rows) >= 2 and rows[-1]["rtc_ms"] - rows[0]["rtc_ms"] > 60_000:
        drift = (rows[-1]["rtc_ms"] - rows[0]["rtc_ms"]) / (rows[-1]["server_ms"] - rows[0]["server_ms"]) - 1
        c.execute("update devices set drift=? where id=?", (drift, d["id"]))
        c.commit()
    return dict(accepted=accepted, server_ms=t, version=db.content_version())


# ------------------------------------------------------------------ dashboard and simulation helpers
@app.get("/", response_class=HTMLResponse)
def dashboard():
    return DASHBOARD_HTML


@app.get("/sim/state")
def sim_state():
    c = db.conn()
    t = db.now_ms()
    cards = []
    for r in c.execute("select * from cards order by due_ms").fetchall():
        b, st = json.loads(r["body"]), json.loads(r["state"])
        cards.append(dict(id=r["id"], kind=b["kind"], front=b["front"], due_in_ms=r["due_ms"] - t, s=st.get("s"),
                          d=st.get("d"), reps=st.get("reps", 0), lapses=st.get("lapses", 0), rev=r["rev"]))
    revs = [dict(r) for r in c.execute("select * from reviews order by id desc limit 25").fetchall()]
    devs = [dict(r) for r in c.execute("select id, name, fw, battery, charging, last_seen_ms, batch_id, batch_version, "
                                       "cards_left, unsynced, drift from devices").fetchall()]
    codes = [dict(user_code=r["user_code"], age_s=(t - r["created_ms"]) // 1000)
             for r in c.execute("select * from codes where approved=0").fetchall() if t - r["created_ms"] < CODE_TTL_MS]
    due_now, due_today = _due_counts()
    releases = [dict(version=r["version"], channel=r.get("channel")) for r in firmware.releases()]
    return dict(now=t, version=db.content_version(), due_now=due_now, due_today=due_today,
                due_device=_due_device(due_now), app_only=SIM["app_only"], cards=cards, reviews=revs, devices=devs,
                codes=codes, firmware_dir=firmware.DIR, releases=releases)


@app.post("/sim/app_only")
def app_only(n: int = 0):
    """Pretend n of the due cards are picture cards only the app can show (they drop out of due_device)."""
    SIM["app_only"] = max(0, n)
    return dict(app_only=SIM["app_only"])


@app.post("/sim/phone_review")
def phone_review(n: int = 3):
    """Pretend you reviewed n due cards on your phone: grades them Good and bumps the content version, so a device
    holding an older batch sees it's stale on its next check-in."""
    c = db.conn()
    t = db.now_ms()
    rows = c.execute("select id from cards where due_ms<=? order by due_ms limit ?", (t, n)).fetchall()
    for r in rows:
        c.execute("insert into reviews (card_id, grade, source, graded_ms, received_ms) values (?,?,?,?,?)",
                  (r["id"], 3, "phone", t, t))
        c.commit()
        db.apply_review(r["id"], 3, t)
    v = db.bump_version()
    return dict(reviewed=[r["id"] for r in rows], version=v)


@app.post("/sim/add_card")
def add_card():
    c = db.conn()
    used = {json.loads(r["body"])["front"] for r in c.execute("select body from cards").fetchall()}
    pool = [x for x in deck.EXTRA_CARDS if x["front"] not in used]
    if not pool:
        return dict(added=None)
    card = dict(pool[0])
    cid = "n%02d" % (c.execute("select count(*) from cards where id like 'n%'").fetchone()[0] + 1)
    c.execute("insert into cards (id, body, due_ms, state, rev, added_ms) values (?,?,?,?,0,?)",
              (cid, json.dumps(dict(card, image_fn=None)), db.now_ms(), "{}", db.now_ms()))
    c.commit()
    return dict(added=cid, version=db.bump_version())


@app.post("/sim/make_due")
def make_due(n: int = 5):
    c = db.conn()
    t = db.now_ms()
    rows = c.execute("select id from cards where due_ms>? order by due_ms limit ?", (t, n)).fetchall()
    for r in rows:
        c.execute("update cards set due_ms=? where id=?", (t - 1000, r["id"]))
    c.commit()
    return dict(made_due=[r["id"] for r in rows], version=db.bump_version())


@app.post("/sim/reset")
def reset():
    db.reset()
    FRAMES.clear()
    return dict(ok=True)


@app.get("/preview/{card_id}.png")
def preview(card_id: str, view: str = "a"):
    r = db.conn().execute("select * from cards where id=?", (card_id,)).fetchone()
    if not r:
        raise HTTPException(404)
    z, crc, meta = _frame(r)
    img = render.unpack(zlib.decompress(z))
    if view == "q":
        img = render.question_view(img, meta)
    buf = io.BytesIO()
    img.convert("L").save(buf, "PNG")
    return Response(buf.getvalue(), media_type="image/png")
