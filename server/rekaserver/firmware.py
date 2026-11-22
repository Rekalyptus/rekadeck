"""Firmware releases for over-the-air updates (PROTOCOL.md "Firmware updates").

Point REKASIM_FIRMWARE_DIR at a folder with a manifest.json and the images it names, the same layout a real server
reads its releases from. tools/release.py writes one in release/. The manifest is read again on every request,
so a release dropped into the folder is offered without a restart. An entry whose file is missing, or whose size or
sha256 doesn't match the file, is skipped with a warning: a half-copied release is never offered.
"""
import hashlib
import json
import os
import re

DIR = os.environ.get("REKASIM_FIRMWARE_DIR") or None
APP_SLOT = 0x170000                       # one app slot in firmware/partitions.csv
_VERSION = re.compile(r"\d+(\.\d+)*")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_hashes = {}                              # path -> ((mtime_ns, size), sha256)
_warned = set()


def version_key(v):
    """'0.3.0' -> (0, 3), or None if it isn't dotted numbers. Compared numerically; trailing zeros don't count."""
    if not isinstance(v, str) or not _VERSION.fullmatch(v):
        return None
    parts = [int(x) for x in v.split(".")]
    while parts and parts[-1] == 0:
        parts.pop()
    return tuple(parts)


def _warn(msg):
    if msg not in _warned:
        _warned.add(msg)
        print(f"  firmware: {msg}", flush=True)


def _sha256(path):
    st = os.stat(path)
    key = (st.st_mtime_ns, st.st_size)
    hit = _hashes.get(path)
    if hit and hit[0] == key:
        return hit[1]
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    _hashes[path] = (key, h.hexdigest())
    return h.hexdigest()


def _problem(r):
    """Why this manifest entry can't be offered, or None if it's good."""
    if not isinstance(r, dict):
        return "not an object"
    if version_key(r.get("version")) is None:
        return "version isn't dotted numbers"
    fn = r.get("file")
    if not isinstance(fn, str) or not fn or os.path.basename(fn) != fn or fn.startswith("."):
        return "file isn't a plain file name"
    size, sha = r.get("size"), r.get("sha256")
    if not isinstance(size, int) or isinstance(size, bool) or not 0 < size <= APP_SLOT:
        return "size is missing or bigger than an app slot"
    if not isinstance(sha, str) or not _SHA256.fullmatch(sha.lower()):
        return "sha256 isn't 64 hex characters"
    path = os.path.join(DIR, fn)
    try:
        actual = os.path.getsize(path)
    except OSError:
        return f"{fn} is missing"
    if actual != size:
        return f"{fn} is {actual} bytes, the manifest says {size}"
    if _sha256(path) != sha.lower():
        return f"{fn} doesn't match its sha256"
    return None


def releases():
    """Every entry in the manifest that checks out against its file."""
    if not DIR:
        return []
    path = os.path.join(DIR, "manifest.json")
    try:
        with open(path) as f:
            data = json.load(f)
    except FileNotFoundError:
        _warn(f"no {path}, so no updates are offered")
        return []
    except (OSError, ValueError) as e:
        _warn(f"can't read {path} ({e}), so no updates are offered")
        return []
    items = data.get("releases") if isinstance(data, dict) else None
    if not isinstance(items, list):
        _warn(f'{path} isn\'t {{"releases": [...]}}, so no updates are offered')
        return []
    good = []
    for r in items:
        why = _problem(r)
        if why:
            _warn(f"skipping {r.get('version') if isinstance(r, dict) else r!r}: {why}")
        else:
            good.append(dict(r, sha256=r["sha256"].lower()))
    return good


def offer(device_fw):
    """hello's `firmware`: the newest stable release newer than the device's own version, or None."""
    mine = version_key(device_fw)
    if mine is None:
        return None
    best = None
    for r in releases():
        k = version_key(r["version"])
        if r.get("channel") == "stable" and k > mine and (best is None or k > version_key(best["version"])):
            best = r
    if not best:
        return None
    return dict(version=best["version"], url=f"/api/device/firmware/{best['version']}", size=best["size"],
                sha256=best["sha256"])


def image(version):
    """(bytes, sha256) of the release with exactly this version, or None. The bytes are hashed again as they're read,
    so what's served always matches the sha256 it's served with."""
    for r in releases():
        if r["version"] == version:
            with open(os.path.join(DIR, r["file"]), "rb") as f:
                data = f.read()
            if len(data) == r["size"] and hashlib.sha256(data).hexdigest() == r["sha256"]:
                return data, r["sha256"]
            _warn(f"{r['file']} changed while it was being served")
            return None
    return None
