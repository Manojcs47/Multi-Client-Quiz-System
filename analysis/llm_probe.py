#!/usr/bin/env python3
"""
LLM backend threshold probe  (cs23btech11047)

Answers Part 2 question 4(a): how many /generate_quiz requests can the LLM
instance actually serve per second, and how does its latency grow with
concurrency? Run this ON THE SERVER (192.168.50.111) - the LLM is only
reachable from there.

    python3 analysis/llm_probe.py --concurrency 1 2 4 8 --requests 8
    python3 analysis/llm_probe.py --host 127.0.0.1 --port 25555   # mock LLM

Each level is reported as mean/p95 latency and completed requests per
second, which is the number to quote as the LLM threshold in the report.
"""
import argparse
import json
import statistics as st
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

SYSTEM = ("You are a trivia generator. Given a genre, output exactly 10 "
          "multiple-choice questions. Each question must have 4 options "
          "(A, B, C, D) and one correct answer. Return ONLY valid JSON.")


def build(rollno, genre, n):
    return json.dumps({
        "rollno": rollno,
        "messages": [
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": f"Generate {n} MCQ trivia questions about {genre}."},
        ],
        "temperature": 0.7,
        "max_tokens": 2000 if n > 1 else 400,
    }).encode()


def post(url, body, timeout):
    req = urllib.request.Request(url, data=body,
                                 headers={"Content-Type": "application/json"})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            payload = r.read()
        return time.time() - t0, r.status, len(payload)
    except urllib.error.HTTPError as e:
        return time.time() - t0, e.code, 0
    except Exception as e:                       # noqa: BLE001
        return time.time() - t0, str(e), 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.50.142")
    ap.add_argument("--port", type=int, default=25000)
    ap.add_argument("--rollno", default="cs23btech11047")
    ap.add_argument("--genre", default="Science")
    ap.add_argument("--questions", type=int, default=10)
    ap.add_argument("--concurrency", type=int, nargs="+", default=[1, 2, 4, 8])
    ap.add_argument("--requests", type=int, default=8,
                    help="requests issued at each concurrency level")
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    base = f"http://{args.host}:{args.port}"
    reserve = json.dumps({"rollno": args.rollno}).encode()
    dt, status, _ = post(base + "/reserve", reserve, 30)
    print(f"/reserve -> {status} in {dt*1000:.0f} ms")
    if status != 200:
        sys.exit("could not reserve the instance; nothing else will work")

    body = build(args.rollno, args.genre, args.questions)
    print(f"\n{'conc':>5} {'ok':>4} {'fail':>5} {'mean ms':>9} {'p95 ms':>8} "
          f"{'wall s':>7} {'req/s':>7}")

    for c in args.concurrency:
        post(base + "/reserve", reserve, 30)     # keep the slot alive
        t0 = time.time()
        with ThreadPoolExecutor(max_workers=c) as pool:
            results = list(pool.map(lambda _: post(base + "/generate_quiz", body,
                                                   args.timeout),
                                    range(args.requests)))
        wall = time.time() - t0
        ok = [r[0] * 1000 for r in results if r[1] == 200]
        fail = len(results) - len(ok)
        p95 = sorted(ok)[int(0.95 * (len(ok) - 1))] if ok else 0
        print(f"{c:>5} {len(ok):>4} {fail:>5} {st.mean(ok) if ok else 0:>9.0f} "
              f"{p95:>8.0f} {wall:>7.1f} {len(ok)/wall if wall else 0:>7.2f}")

    print("\nThe req/s column stops growing once the backend saturates; that "
          "plateau is the LLM threshold to quote in the report.")


if __name__ == "__main__":
    main()
