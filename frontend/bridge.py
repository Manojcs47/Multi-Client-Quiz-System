#!/usr/bin/env python3
"""
CS3530 Assignment-1 - browser bridge (cs23btech11047)

A browser cannot open a raw TCP socket, so this small gateway sits on the
client host and translates:

    browser  <--HTTP/SSE-->  bridge  <--custom TCP protocol-->  quiz server

It opens one TCP connection per browser session, frames every message with
the same 12-byte header the C++ client uses, answers KEEP_ALIVE probes on
behalf of the page, and streams everything it sees to the browser as
Server-Sent Events (including the raw frame metadata, which is what the
"wire" panel in the UI displays).

Only the Python standard library is used.

    python3 frontend/bridge.py --server-host 192.168.50.111 --server-port 11047
    then open http://localhost:8080
"""
import argparse
import json
import os
import queue
import socket
import struct
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

MAGIC = 0x515A
VERSION = 1
HEADER = "!HBBII"
HEADER_SIZE = 12

TYPE = {
    "GENRE_REQUEST": 1, "QUESTION_DELIVERY": 2, "ANSWER_SUBMISSION": 3,
    "RESULT_DELIVERY": 4, "LEADERBOARD_REQUEST": 5, "LEADERBOARD_DELIVERY": 6,
    "SESSION_END": 7, "KEEP_ALIVE": 8, "ALIVE_OK": 9, "USERNAME_SUBMISSION": 10,
    "ERROR_MSG": 11, "SERVER_INFO": 12,
}
NAME = {v: k for k, v in TYPE.items()}

FLAG_CACHE_HIT = 0x01
FLAG_CACHE_MISS = 0x02

SESSIONS = {}
LOCK = threading.Lock()
HERE = os.path.dirname(os.path.abspath(__file__))
ARGS = None


def frame(msg_type, payload="", flags=0):
    body = payload.encode("utf-8")
    return struct.pack(HEADER, MAGIC, VERSION, flags, msg_type, len(body)) + body


def recv_exactly(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


class Session:
    def __init__(self, username, genre, host, port):
        self.id = uuid.uuid4().hex[:12]
        self.username = username
        self.genre = genre or "General Knowledge"
        self.events = queue.Queue()
        self.alive = True
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(None)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.send_lock = threading.Lock()
        self.bytes_in = 0
        self.bytes_out = 0
        self.started = time.time()
        self.send("USERNAME_SUBMISSION", username)
        threading.Thread(target=self._reader, daemon=True).start()

    # ---------------- outgoing ----------------
    def send(self, type_name, payload=""):
        code = TYPE[type_name]
        data = frame(code, payload)
        with self.send_lock:
            self.sock.sendall(data)
        self.bytes_out += len(data)
        self.push({"dir": "out", "type": type_name, "flags": 0,
                   "bytes": len(data), "payload": payload})

    def push(self, event):
        event["t"] = time.time()
        event["bytes_in"] = self.bytes_in
        event["bytes_out"] = self.bytes_out
        self.events.put(event)

    # ---------------- incoming ----------------
    def _reader(self):
        try:
            while self.alive:
                head = recv_exactly(self.sock, HEADER_SIZE)
                if head is None:
                    break
                magic, version, flags, mtype, length = struct.unpack(HEADER, head)
                if magic != MAGIC or version != VERSION:
                    self.push({"dir": "in", "type": "BAD_FRAME", "flags": 0,
                               "bytes": HEADER_SIZE, "payload": ""})
                    break
                body = b""
                if length:
                    body = recv_exactly(self.sock, length)
                    if body is None:
                        break
                self.bytes_in += HEADER_SIZE + length
                text = body.decode("utf-8", "replace")
                try:
                    parsed = json.loads(text) if text else {}
                except ValueError:
                    parsed = {"text": text}

                name = NAME.get(mtype, f"TYPE_{mtype}")
                if mtype == TYPE["KEEP_ALIVE"]:
                    # answer on behalf of the page so a slow browser is
                    # never mistaken for a dead client
                    self.send("ALIVE_OK")

                self.push({"dir": "in", "type": name, "flags": flags,
                           "cache": "hit" if flags & FLAG_CACHE_HIT else
                                    ("miss" if flags & FLAG_CACHE_MISS else None),
                           "bytes": HEADER_SIZE + length, "payload": parsed})

                if mtype == TYPE["SESSION_END"]:
                    break
        except OSError:
            pass
        finally:
            self.alive = False
            self.push({"dir": "in", "type": "CLOSED", "flags": 0, "bytes": 0,
                       "payload": {}})

    def close(self):
        self.alive = False
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "quiz-bridge/1.0"

    def log_message(self, fmt, *args):
        if ARGS and ARGS.verbose:
            print("[bridge] " + fmt % args)

    # ------------- helpers -------------
    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length", 0))
        if not n:
            return {}
        try:
            return json.loads(self.rfile.read(n))
        except ValueError:
            return {}

    def _static(self, name, ctype):
        path = os.path.join(HERE, name)
        if not os.path.exists(path):
            self._send(404, {"error": f"{name} is missing next to bridge.py"})
            return
        with open(path, "rb") as fh:
            self._send(200, fh.read(), ctype)

    # ------------- routes -------------
    def do_GET(self):
        url = urlparse(self.path)
        if url.path in ("/", "/index.html"):
            return self._static("index.html", "text/html; charset=utf-8")
        if url.path == "/api/health":
            return self._send(200, {"ok": True, "server":
                                    f"{ARGS.server_host}:{ARGS.server_port}"})
        if url.path == "/api/events":
            return self.stream(parse_qs(url.query).get("sid", [""])[0])
        self._send(404, {"error": "not found"})

    def do_POST(self):
        url = urlparse(self.path)
        body = self._body()

        if url.path == "/api/connect":
            username = (body.get("username") or "player").strip()[:32]
            genre = (body.get("genre") or "").strip()[:64]
            try:
                s = Session(username, genre, ARGS.server_host, ARGS.server_port)
            except OSError as exc:
                return self._send(502, {"error":
                    f"Cannot reach the quiz server at {ARGS.server_host}:"
                    f"{ARGS.server_port} ({exc.strerror or exc}). "
                    "Start the C++ server, then try again."})
            with LOCK:
                SESSIONS[s.id] = s
            print(f"[bridge] session {s.id} for '{username}' -> "
                  f"{ARGS.server_host}:{ARGS.server_port}")
            return self._send(200, {"sid": s.id, "username": username,
                                    "genre": s.genre})

        if url.path == "/api/send":
            sid = body.get("sid", "")
            with LOCK:
                s = SESSIONS.get(sid)
            if not s or not s.alive:
                return self._send(410, {"error": "session is closed"})
            type_name = body.get("type", "")
            if type_name not in TYPE:
                return self._send(400, {"error": f"unknown type {type_name}"})
            try:
                s.send(type_name, body.get("payload", ""))
            except OSError as exc:
                return self._send(502, {"error": str(exc)})
            return self._send(200, {"ok": True})

        if url.path == "/api/disconnect":
            with LOCK:
                s = SESSIONS.pop(body.get("sid", ""), None)
            if s:
                s.close()
            return self._send(200, {"ok": True})

        self._send(404, {"error": "not found"})

    # ------------- SSE -------------
    def stream(self, sid):
        with LOCK:
            s = SESSIONS.get(sid)
        if not s:
            return self._send(404, {"error": "unknown session"})

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        try:
            while True:
                try:
                    event = s.events.get(timeout=10)
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")     # keeps proxies happy
                    self.wfile.flush()
                    continue
                self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())
                self.wfile.flush()
                if event.get("type") == "CLOSED":
                    break
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with LOCK:
                if not s.alive:
                    SESSIONS.pop(sid, None)


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--server-host", default="192.168.50.111",
                    help="address of the C++ quiz server")
    ap.add_argument("--server-port", type=int, default=11047)
    ap.add_argument("--listen", type=int, default=8080, help="web UI port")
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--verbose", action="store_true")
    ARGS = ap.parse_args()

    httpd = ThreadingHTTPServer((ARGS.bind, ARGS.listen), Handler)
    print(f"Quiz web UI  : http://{ARGS.bind}:{ARGS.listen}")
    print(f"Quiz server  : {ARGS.server_host}:{ARGS.server_port}")
    print("Ctrl-C to stop")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbridge stopped")


if __name__ == "__main__":
    main()
