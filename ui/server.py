#!/usr/bin/env python3
"""SeedForge web UI — http://localhost:7070"""

import http.server
import json
import os
import queue
import re
import select as _select
import subprocess
import tempfile
import threading
import time
from pathlib import Path

PORT = 7070
ROOT = Path(__file__).parent.parent   # project root

# ── SSE broadcast ─────────────────────────────────────────────────────────────

_clients: list[queue.Queue] = []
_clients_lock = threading.Lock()


def _broadcast(event: str, data: dict):
    msg = f"event: {event}\ndata: {json.dumps(data)}\n\n"
    with _clients_lock:
        dead = []
        for q in _clients:
            try:
                q.put_nowait(msg)
            except queue.Full:
                dead.append(q)
        for q in dead:
            _clients.remove(q)


def _add_client(q: queue.Queue):
    with _clients_lock:
        _clients.append(q)


def _remove_client(q: queue.Queue):
    with _clients_lock:
        try:
            _clients.remove(q)
        except ValueError:
            pass


# ── Run state ─────────────────────────────────────────────────────────────────

_proc: subprocess.Popen | None = None
_proc_lock = threading.Lock()
_state: dict = {"state": "idle", "pct": 0.0, "speed": 0.0, "eta": "", "hits": 0, "error": ""}


def _set_state(**kw):
    _state.update(kw)
    _broadcast("status", dict(_state))


# ── Stderr progress parser ─────────────────────────────────────────────────────

_BAR_RE = re.compile(r"(\d+\.\d+)%.*?(\d+\.\d+)\s*M/s(?:.*?ETA\s*(\d+)m(\d+)s)?")


def _parse_stderr_line(line: str):
    m = _BAR_RE.search(line)
    if not m:
        return
    pct   = float(m.group(1))
    speed = float(m.group(2))
    eta   = f"{m.group(3)}m {m.group(4)}s" if m.group(3) else ""
    _state["pct"]   = pct
    _state["speed"] = speed
    _state["eta"]   = eta
    _broadcast("progress", {"pct": pct, "speed": speed, "eta": eta})


def _drain_stderr(proc: subprocess.Popen):
    """Read stderr in binary chunks via os.read to avoid TextIOWrapper buffering."""
    fd = proc.stderr.fileno()
    buf = b""
    while True:
        try:
            ready, _, _ = _select.select([fd], [], [], 0.3)
        except (ValueError, OSError):
            break
        if ready:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            # split on \r or \n, parse all complete lines
            parts = re.split(rb"[\r\n]+", buf)
            for line in parts[:-1]:
                txt = line.decode("utf-8", errors="replace").strip()
                if txt:
                    _parse_stderr_line(txt)
            buf = parts[-1]
        elif proc.poll() is not None:
            break
    if buf:
        txt = buf.decode("utf-8", errors="replace").strip()
        if txt:
            _parse_stderr_line(txt)


# ── Hits file watcher ─────────────────────────────────────────────────────────

def _watch_hits(path: str, stop: threading.Event):
    deadline = time.monotonic() + 15
    while not os.path.exists(path):
        if stop.is_set() or time.monotonic() > deadline:
            return
        time.sleep(0.05)

    hits = 0
    with open(path) as f:
        while not stop.is_set():
            line = f.readline()
            if line:
                parts = line.strip().split("\t")
                if len(parts) >= 3:
                    hits += 1
                    _state["hits"] = hits
                    try:
                        _broadcast("hit", {
                            "n": hits,
                            "seed": parts[0],
                            "x": int(parts[1]),
                            "z": int(parts[2]),
                            # 4th column (optional) = measured biome size in blocks
                            "size": int(parts[3]) if len(parts) >= 4 else 0,
                        })
                    except (ValueError, IndexError):
                        pass
            else:
                time.sleep(0.08)


# ── Search runner ─────────────────────────────────────────────────────────────

def _run_search(cfg_text: str, use_gpu: bool):
    global _proc

    binary = ROOT / ("seedforge_gpu" if use_gpu else "seedforge_cpu")
    if not binary.exists():
        _set_state(state="error", error=f"Binary not found: {binary.name} — run make first")
        return

    # Resolve output path from config text
    out_path = ROOT / "hits.txt"
    for raw in cfg_text.splitlines():
        m = re.match(r"out\s*=\s*(.+)", raw.strip())
        if m:
            p = m.group(1).strip()
            out_path = Path(p) if os.path.isabs(p) else ROOT / p
            break

    # Clear the output file before launch so the watcher sees only new hits
    try:
        out_path.write_text("")
    except OSError:
        pass

    # Write config to a temp file in ROOT so relative paths inside it work
    fd, cfg_path = tempfile.mkstemp(suffix=".cfg", dir=str(ROOT))
    try:
        with os.fdopen(fd, "w") as f:
            f.write(cfg_text)

        stop_watch = threading.Event()
        watch_th = threading.Thread(
            target=_watch_hits, args=(str(out_path), stop_watch), daemon=True
        )
        watch_th.start()

        proc = subprocess.Popen(
            [str(binary), cfg_path],
            stderr=subprocess.PIPE,          # parsed for progress
            stdout=subprocess.DEVNULL,       # one-line summary not needed by UI
            cwd=str(ROOT),
        )
        with _proc_lock:
            _proc = proc

        _set_state(state="running", pct=0.0, speed=0.0, eta="", hits=0, error="")

        stderr_th = threading.Thread(target=_drain_stderr, args=(proc,), daemon=True)
        stderr_th.start()

        # Wait for process to exit (natural completion or SIGKILL from /api/stop)
        proc.wait()
        stderr_th.join(timeout=3)

        rc = proc.returncode
        if rc == 0:
            _set_state(state="done", pct=100.0, eta="")
        elif rc in (-9, -15):   # SIGKILL / SIGTERM from /api/stop
            _set_state(state="stopped")
        else:
            _set_state(state="error", error=f"binary exited with code {rc}")

    except Exception as exc:
        _set_state(state="error", error=str(exc))
    finally:
        stop_watch.set()
        watch_th.join(timeout=3)
        try:
            os.unlink(cfg_path)
        except OSError:
            pass
        with _proc_lock:
            _proc = None

    # Final hit count
    try:
        n = sum(1 for ln in out_path.read_text().splitlines() if ln.strip())
        _state["hits"] = n
    except OSError:
        pass
    _broadcast("done", {"hits": _state["hits"]})


# ── HTTP handler ──────────────────────────────────────────────────────────────

_HTML = Path(__file__).parent / "index.html"


class _Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass  # silence access log

    def _send(self, code: int, ctype: str, body: bytes):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]

        if path in ("/", "/index.html"):
            self._send(200, "text/html; charset=utf-8", _HTML.read_bytes())

        elif path == "/api/status":
            self._send(200, "application/json", json.dumps(_state).encode())

        elif path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()

            q: queue.Queue = queue.Queue(maxsize=500)
            _add_client(q)
            try:
                # Immediately push current state
                init = f"event: status\ndata: {json.dumps(_state)}\n\n"
                self.wfile.write(init.encode())
                self.wfile.flush()
                while True:
                    try:
                        msg = q.get(timeout=15)
                        self.wfile.write(msg.encode())
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(b": ping\n\n")
                        self.wfile.flush()
            except Exception:
                pass
            finally:
                _remove_client(q)

        else:
            self._send(404, "text/plain", b"not found")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)

        if self.path == "/api/start":
            with _proc_lock:
                busy = _proc is not None and _proc.poll() is None
            if busy:
                self._send(409, "application/json", b'{"error":"already running"}')
                return
            try:
                req = json.loads(body)
            except Exception:
                self._send(400, "application/json", b'{"error":"bad json"}')
                return
            cfg  = req.get("cfg", "")
            gpu  = bool(req.get("gpu", False))
            threading.Thread(target=_run_search, args=(cfg, gpu), daemon=True).start()
            self._send(200, "application/json", b'{"ok":true}')

        elif self.path == "/api/stop":
            with _proc_lock:
                p = _proc
            if p and p.poll() is None:
                p.kill()   # SIGKILL — SIGTERM can be swallowed by OpenMP workers
            self._send(200, "application/json", b'{"ok":true}')

        else:
            self._send(404, "text/plain", b"not found")

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()


if __name__ == "__main__":
    server = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), _Handler)
    print(f"SeedForge UI  →  http://localhost:{PORT}")
    print("Ctrl-C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
