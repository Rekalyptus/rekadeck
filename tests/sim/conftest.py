"""Fixtures for the simulator scenarios: a mock server and a simulator (--fast) per test, on free ports, with fresh
data, driven through the simulator's HTTP control API the way its web page drives it.

Needs a built simulator (sim/build/rekadeck_sim, or REKADECK_SIM) and the mock server's requirements installed in
the Python running pytest:

    pip install -r server/requirements.txt pytest
    pytest tests/sim -q
"""

import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SIM = os.environ.get("REKADECK_SIM", os.path.join(ROOT, "sim", "build", "rekadeck_sim"))


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_http(url: str, timeout: float = 20) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            urllib.request.urlopen(url, timeout=1)
            return
        except urllib.error.HTTPError:
            return   # it answered
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(url)


class Sim:
    """The simulator's control API, plus a log reader that remembers what it has already seen."""

    def __init__(self, port: int, server_url: str):
        self.base = f"http://127.0.0.1:{port}"
        self.server = server_url
        self.seen = 0
        self.lines: list[str] = []     # not yet matched by wait_log
        self.history: list[str] = []   # everything, for logged()

    def _post(self, path: str, body: bytes = b"") -> bytes:
        req = urllib.request.Request(self.base + path, data=body, method="POST")
        return urllib.request.urlopen(req, timeout=10).read()

    def _get(self, path: str):
        return json.loads(urllib.request.urlopen(self.base + path, timeout=10).read())

    def key(self, k: int, hold_ms: int = 80, then: float = 0.6) -> None:
        self._post(f"/api/key?k={k}&down=1")
        time.sleep(hold_ms / 1000)
        self._post(f"/api/key?k={k}&down=0")
        time.sleep(then)

    def menu(self) -> None:
        """Hold keys 1 and 4 for the menu."""
        self._post("/api/key?k=1&down=1")
        self._post("/api/key?k=4&down=1")
        time.sleep(2.4)
        self._post("/api/key?k=1&down=0")
        self._post("/api/key?k=4&down=0")
        self.wait_log("key menu")

    def settle(self, timeout: float = 15) -> None:
        """Wait until the panel has finished refreshing, as a person waits to see a screen before answering it.
        A full-screen refresh blocks for 1.8 s or more; taps during it are queued, not lost, but a test that answers
        a screen before it's up is testing the wrong thing."""
        time.sleep(0.15)   # let a refresh the last tap set off begin
        deadline = time.time() + timeout
        while self.state()["refreshing_ms"] > 0:
            if time.time() > deadline:
                raise AssertionError("the panel never finished refreshing")
            time.sleep(0.05)

    def menu_pick(self, ups: int) -> None:
        """From a fresh menu (on "Back to cards"), go up `ups` times (it wraps) and select."""
        for _ in range(ups):
            self.key(1, then=0.4)
        self.key(3, then=0)
        self.settle()

    def control(self, **kw) -> dict:
        return json.loads(self._post("/api/control", json.dumps(kw).encode()))

    def state(self) -> dict:
        return self._get("/api/state")

    def refreshes(self) -> list:
        return self._get("/api/refreshes")

    def poll_log(self) -> list[str]:
        new = self._get(f"/api/log?after={self.seen}")
        for e in new:
            self.seen = max(self.seen, e["n"])
            self.lines.append(e["t"])
            self.history.append(e["t"])
        return [e["t"] for e in new]

    def wait_log(self, text: str, timeout: float = 30) -> str:
        """Wait for a log line containing text, from anything logged after the last line matched."""
        deadline = time.time() + timeout
        while True:
            for i, line in enumerate(self.lines):
                if text in line:
                    del self.lines[: i + 1]   # later waits look past this one
                    return line
            if time.time() > deadline:
                raise AssertionError(f"no log line with {text!r}; recent lines:\n  " + "\n  ".join(self.lines[-25:]))
            self.poll_log()
            time.sleep(0.1)

    def mark(self) -> int:
        """A point in the log, for logged(since=...)."""
        self.poll_log()
        return len(self.history)

    def logged(self, text: str, since: int = 0) -> bool:
        """Anything logged so far (or after a mark), matched by wait_log or not."""
        self.poll_log()
        return any(text in line for line in self.history[since:])

    def wait_state(self, key: str, value, timeout: float = 20) -> None:
        deadline = time.time() + timeout
        while self.state().get(key) != value:
            if time.time() > deadline:
                raise AssertionError(f"state[{key!r}] never became {value!r}")
            time.sleep(0.1)


@pytest.fixture
def mock_server(tmp_path, request):
    """The mock Rekalyptus server on a free port. Tests can set extra env through the `server_env` marker."""
    port = free_port()
    env = dict(os.environ, PORT=str(port), REKASIM_DB=str(tmp_path / "rekasim.db"))
    marker = request.node.get_closest_marker("server_env")
    if marker:
        env.update({k: (v(tmp_path) if callable(v) else v) for k, v in marker.kwargs.items()})
    proc = subprocess.Popen([sys.executable, "-m", "rekaserver"], cwd=os.path.join(ROOT, "server"), env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        wait_http(f"http://127.0.0.1:{port}/")
        yield f"http://127.0.0.1:{port}"
    finally:
        proc.terminate()
        proc.wait(timeout=10)


@pytest.fixture
def sim(tmp_path, mock_server):
    """A simulator that joins "Home WiFi" and links itself to the mock server, already showing its first card."""
    if not os.path.exists(SIM):
        pytest.skip(f"simulator not built at {SIM}")
    data = tmp_path / "sim"
    data.mkdir()
    (data / "settings.json").write_text(json.dumps({"server": mock_server, "ssid": "Home WiFi", "pass": "flashcards"}))
    port = free_port()
    proc = subprocess.Popen([SIM, "--port", str(port), "--data", str(data), "--fast"], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        wait_http(f"http://127.0.0.1:{port}/api/state")
        s = Sim(port, mock_server)
        s.wait_log("new batch ready", timeout=60)
        s.wait_log("wifi off until the next check-in")
        yield s
    finally:
        proc.terminate()
        proc.wait(timeout=10)


def pytest_configure(config):
    config.addinivalue_line("markers", "server_env(**env): extra environment for the mock server")
