#!/usr/bin/env python3
"""
Stand-in for the course LLM backend (192.168.50.142:25000).

It speaks the same two endpoints, /reserve and /generate_quiz, and returns
questions in the same envelope, so the quiz servers can be developed and
measured when the shared instance is busy or unreachable.

    python3 tools/mock_llm.py --port 25000 --delay 1.5
    QUIZ_LLM_HOST=127.0.0.1 ./server        # in another terminal

--delay fakes LLM generation time, which is what makes the Part A vs
Part B cache comparison visible.
"""
import argparse
import json
import random
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DELAY = 1.5

TOPICS = {
    "default": [
        ("What is the chemical symbol for {x}?", ["Au", "Ag", "Fe", "Cu"]),
        ("Which of these is closest to {x}?", ["Alpha", "Beta", "Gamma", "Delta"]),
        ("In which year did {x} happen?", ["1492", "1776", "1865", "1945"]),
        ("Who is most associated with {x}?", ["Curie", "Newton", "Darwin", "Tesla"]),
        ("Which continent is linked to {x}?", ["Asia", "Europe", "Africa", "America"]),
    ]
}


def make_questions(genre, n):
    out = []
    templates = TOPICS["default"]
    for i in range(n):
        text, opts = templates[i % len(templates)]
        stem = text.format(x=f"{genre} topic {random.randint(1, 9999)}")
        letters = ["A", "B", "C", "D"]
        options = [f"{letters[k]}) {opts[k]}" for k in range(4)]
        out.append({"q": stem, "options": options, "a": random.choice(letters)})
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[mock-llm] " + fmt % args)

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        try:
            req = json.loads(self.rfile.read(length) or b"{}")
        except Exception:
            req = {}

        if self.path == "/reserve":
            self._json(200, {"status": "reserved", "rollno": req.get("rollno")})
            return

        if self.path == "/generate_quiz":
            msgs = req.get("messages", [])
            prompt = msgs[-1].get("content", "") if msgs else ""
            genre = prompt.split("about")[-1].strip(" .") or "general"
            count = 10 if "10 MCQ" in prompt else 1
            time.sleep(DELAY)
            payload = {"questions": make_questions(genre, count)}
            self._json(200, {"choices": [{"message": {"role": "assistant",
                                                      "content": json.dumps(payload)}}]})
            return

        self._json(404, {"error": "unknown endpoint"})


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=25000)
    ap.add_argument("--delay", type=float, default=1.5,
                    help="simulated generation time in seconds")
    args = ap.parse_args()
    DELAY = args.delay
    print(f"mock LLM on http://127.0.0.1:{args.port} (delay {DELAY}s)")
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()
