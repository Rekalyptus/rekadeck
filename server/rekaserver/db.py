"""SQLite state for the mock server: cards with their schedule, reviews, devices, link codes, clock anchors."""
import json
import os
import random
import sqlite3
import threading
import time

from . import deck, fsrs

PATH = os.environ.get("REKASIM_DB", os.path.join(os.path.dirname(os.path.dirname(__file__)), "rekasim.db"))
_lock = threading.RLock()
_conn = None


def now_ms():
    return int(time.time() * 1000)


def conn():
    global _conn
    if _conn is None:
        _conn = sqlite3.connect(PATH, check_same_thread=False)
        _conn.row_factory = sqlite3.Row
        _init()
    return _conn


SCHEMA = """
create table if not exists cards (id text primary key, body text, due_ms integer, state text, rev integer default 0,
                                  added_ms integer);
create table if not exists reviews (id integer primary key autoincrement, card_id text, grade integer, source text,
                                    device_id text, rid integer, boot integer, shown_ms integer, revealed_ms integer,
                                    graded_ms integer, choice integer, received_ms integer, unique(device_id, rid));
create table if not exists devices (id text primary key, token text unique, name text, fw text, battery real,
                                    charging integer, last_seen_ms integer, batch_id text, batch_version integer,
                                    cards_left integer, unsynced integer, drift real, created_ms integer);
create table if not exists codes (device_code text primary key, user_code text, created_ms integer, approved integer,
                                  device_id text);
create table if not exists anchors (device_id text, boot integer, rtc_ms integer, server_ms integer);
create table if not exists meta (k text primary key, v text);
"""


def _init():
    c = _conn
    c.executescript(SCHEMA)
    if c.execute("select count(*) from cards").fetchone()[0] == 0:
        seed(c)


def seed(c):
    """Load the sample deck with a believable schedule: most cards due now, some later."""
    rnd = random.Random(7)
    t = now_ms()
    for i, card in enumerate(deck.CARDS):
        body = {k: v for k, v in card.items() if k not in ("image",)}
        body["image_fn"] = card["image"].__name__ if card.get("image") else None
        if i % 5 == 4:                              # a few brand-new cards
            state, due = {}, t - rnd.randint(0, 3600_000)
        else:
            s = rnd.uniform(1.5, 25)
            state = dict(s=s, d=rnd.uniform(3, 8), last_ms=t - int(s * fsrs.DAY_MS * rnd.uniform(0.8, 1.3)), reps=rnd.randint(1, 6), lapses=rnd.randint(0, 2))
            due = t - rnd.randint(0, 3 * fsrs.DAY_MS) if i % 3 else t + rnd.randint(1, 5) * fsrs.DAY_MS
        c.execute("insert into cards (id, body, due_ms, state, rev, added_ms) values (?,?,?,?,0,?)",
                  (card["id"], json.dumps(body), due, json.dumps(state), t))
    c.execute("insert or replace into meta values ('content_version', '1')")
    c.commit()


def reset():
    with _lock:
        c = conn()
        for tbl in ("cards", "reviews", "devices", "codes", "anchors", "meta"):
            c.execute(f"delete from {tbl}")
        seed(c)


def meta(k, default=None):
    r = conn().execute("select v from meta where k=?", (k,)).fetchone()
    return r["v"] if r else default


def set_meta(k, v):
    conn().execute("insert or replace into meta values (?,?)", (k, str(v)))
    conn().commit()


def content_version():
    return int(meta("content_version", 1))


def bump_version():
    with _lock:
        v = content_version() + 1
        set_meta("content_version", v)
        return v


def card_full(row):
    """DB row -> card dict with its drawing function put back."""
    body = json.loads(row["body"])
    fn = body.pop("image_fn", None)
    if fn:
        body["image"] = getattr(deck, fn)
    body["id"] = row["id"]
    return body


def apply_review(card_id, grade, when_ms):
    """Schedule one review (grade 1-4). Returns False if the card doesn't exist."""
    with _lock:
        c = conn()
        r = c.execute("select * from cards where id=?", (card_id,)).fetchone()
        if not r:
            return False
        state, due = fsrs.next_state(json.loads(r["state"]), grade, when_ms)
        c.execute("update cards set state=?, due_ms=?, rev=rev+1 where id=?", (json.dumps(state), due, card_id))
        c.commit()
        return True
