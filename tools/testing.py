#!/usr/bin/env python3
"""
Python load-test client for CS3530 Assignment-1  (cs23btech11047)

Equivalent to `./client --auto`, kept because it is handy for quick checks
from a machine where the C++ client has not been built. It speaks the same
12-byte framing:  struct.pack('!HBBII', 0x515A, 1, flags, type, length)

    python3 tools/testing.py --clients 5 --duration 60
"""
import argparse
import json
import random
import socket
import struct
import threading
import time

MAGIC, VERSION, HEADER_SIZE = 0x515A, 1, 12
GENRES = ["Science", "History", "Maths", "Geography", "Movies",
          "Music", "Sports", "Literature", "Art", "Technology"]


class T:
    GENRE_REQUEST = 1; QUESTION_DELIVERY = 2; ANSWER_SUBMISSION = 3
    RESULT_DELIVERY = 4; LEADERBOARD_REQUEST = 5; LEADERBOARD_DELIVERY = 6
    SESSION_END = 7; KEEP_ALIVE = 8; ALIVE_OK = 9; USERNAME_SUBMISSION = 10
    ERROR_MSG = 11; SERVER_INFO = 12


NAME = {v: k for k, v in vars(T).items() if isinstance(v, int)}


def send(sock, mtype, payload=""):
    body = payload.encode()
    sock.sendall(struct.pack("!HBBII", MAGIC, VERSION, 0, mtype, len(body)) + body)


def recv_exactly(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_message(sock):
    head = recv_exactly(sock, HEADER_SIZE)
    if head is None:
        return None
    magic, version, flags, mtype, length = struct.unpack("!HBBII", head)
    if magic != MAGIC or version != VERSION:
        raise ValueError("bad frame")
    body = recv_exactly(sock, length) if length else b""
    if body is None:
        return None
    return mtype, flags, body.decode("utf-8", "replace")


def run_client(name, args, stats):
    genre = random.choice(GENRES) if args.mixed_genres else args.genre
    try:
        sock = socket.create_connection((args.host, args.port), timeout=10)
    except OSError as exc:
        print(f"[{name}] cannot connect: {exc}")
        return
    sock.settimeout(30)
    send(sock, T.USERNAME_SUBMISSION, name)

    t0 = time.time()
    sent_at = time.time()
    send(sock, T.GENRE_REQUEST, genre)
    latencies = []

    try:
        while time.time() - t0 < args.duration:
            msg = recv_message(sock)
            if msg is None:
                break
            mtype, flags, payload = msg

            if mtype == T.KEEP_ALIVE:
                send(sock, T.ALIVE_OK)

            elif mtype == T.QUESTION_DELIVERY:
                latencies.append((time.time() - sent_at) * 1000)
                data = json.loads(payload or "{}")
                if "error" in data:
                    time.sleep(2)
                    genre = random.choice(GENRES)
                    sent_at = time.time()
                    send(sock, T.GENRE_REQUEST, genre)
                    continue
                cache = "cache" if flags & 0x01 else "fresh"
                print(f"[{name}] Q{data.get('question_id','?')} "
                      f"{latencies[-1]:.0f} ms ({cache})")
                time.sleep(random.uniform(1.0, 3.0))
                send(sock, T.ANSWER_SUBMISSION, random.choice("ABCD"))

            elif mtype == T.RESULT_DELIVERY:
                time.sleep(random.uniform(0.5, 1.5))
                sent_at = time.time()
                send(sock, T.GENRE_REQUEST, genre)

            elif mtype == T.SESSION_END:
                data = json.loads(payload or "{}")
                print(f"[{name}] session over, score {data.get('final_score', 0)}")
                break
    except (OSError, ValueError) as exc:
        print(f"[{name}] {exc}")
    finally:
        try:
            send(sock, T.SESSION_END)
            time.sleep(0.3)
        except OSError:
            pass
        sock.close()
        if latencies:
            stats.append((name, len(latencies), sum(latencies) / len(latencies)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.50.111")
    ap.add_argument("--port", type=int, default=11047)
    ap.add_argument("--clients", type=int, default=3)
    ap.add_argument("--duration", type=int, default=60)
    ap.add_argument("--genre", default="Science")
    ap.add_argument("--mixed-genres", action="store_true")
    args = ap.parse_args()

    stats, threads = [], []
    print(f"starting {args.clients} bot(s) for {args.duration}s")
    for i in range(args.clients):
        th = threading.Thread(target=run_client,
                              args=(f"PyBot_{i+1}", args, stats), daemon=False)
        threads.append(th)
        th.start()
        time.sleep(0.2)
    for th in threads:
        th.join()

    print("\nname            questions   mean question latency")
    for name, n, mean in sorted(stats):
        print(f"{name:<15} {n:>9}   {mean:>8.0f} ms")
