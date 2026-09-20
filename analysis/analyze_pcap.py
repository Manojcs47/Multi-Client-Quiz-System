#!/usr/bin/env python3
"""
Trace analysis for CS3530 Assignment-1 Part 2  (cs23btech11047)

Reads a .pcap with tshark, re-assembles the quiz protocol out of the TCP
payloads and reports, per capture:

  * application latency measured from the packets themselves
        question latency = GENRE_REQUEST  -> QUESTION_DELIVERY
        answer latency   = ANSWER_SUBMISSION -> RESULT_DELIVERY
  * effective throughput in bytes/s over the capture window
  * TCP health: retransmissions, zero windows, duplicate ACKs, ACK RTT
  * how many of the questions carried the cache-hit flag

    python3 analysis/analyze_pcap.py pcaps/partb_N10.pcap --port 11047
    python3 analysis/analyze_pcap.py pcaps/*.pcap --csv results/pcap_summary.csv

Needs tshark (sudo apt install tshark).
"""
import argparse
import csv
import os
import shutil
import statistics as st
import subprocess
import sys

MAGIC = "515a"
NAMES = {1: "GENRE_REQUEST", 2: "QUESTION_DELIVERY", 3: "ANSWER_SUBMISSION",
         4: "RESULT_DELIVERY", 5: "LEADERBOARD_REQUEST", 6: "LEADERBOARD_DELIVERY",
         7: "SESSION_END", 8: "KEEP_ALIVE", 9: "ALIVE_OK", 10: "USERNAME_SUBMISSION",
         11: "ERROR_MSG", 12: "SERVER_INFO"}


def tshark(args):
    out = subprocess.run(["tshark"] + args, capture_output=True, text=True)
    if out.returncode != 0 and not out.stdout:
        sys.stderr.write(out.stderr)
    return out.stdout


def count(path, display_filter):
    txt = tshark(["-r", path, "-Y", display_filter, "-T", "fields", "-e", "frame.number"])
    return sum(1 for line in txt.splitlines() if line.strip())


def messages(path, port):
    """Yield (t, src, dst, type, flags, payload_len) for every protocol message."""
    fields = ["frame.time_epoch", "ip.src", "tcp.srcport", "ip.dst", "tcp.dstport",
              "tcp.payload"]
    cmd = ["-r", path, "-Y", f"tcp.port=={port} && tcp.len>0", "-T", "fields"]
    for f in fields:
        cmd += ["-e", f]
    streams = {}
    for line in tshark(cmd).splitlines():
        parts = line.split("\t")
        if len(parts) < 6 or not parts[5]:
            continue
        t = float(parts[0])
        key = (parts[1], parts[2], parts[3], parts[4])
        hexdata = parts[5].replace(":", "")
        streams[key] = streams.get(key, "") + hexdata

        buf = streams[key]
        while len(buf) >= 24:                      # 12 bytes == 24 hex chars
            if buf[0:4] != MAGIC:                  # resynchronise on the magic
                nxt = buf.find(MAGIC, 2)
                if nxt < 0:
                    buf = ""
                    break
                buf = buf[nxt:]
                continue
            mtype = int(buf[8:16], 16)
            plen = int(buf[16:24], 16)
            need = 24 + plen * 2
            if len(buf) < need:
                break
            yield (t, f"{parts[1]}:{parts[2]}", f"{parts[3]}:{parts[4]}",
                   mtype, int(buf[6:8], 16), plen)
            buf = buf[need:]
        streams[key] = buf


def analyse(path, port):
    msgs = list(messages(path, port))
    if not msgs:
        return None

    times = [m[0] for m in msgs]
    window = max(times) - min(times)
    app_bytes = sum(12 + m[5] for m in msgs)

    pending_q, pending_a = {}, {}
    q_lat, a_lat = [], []
    hits = misses = 0

    for t, src, dst, mtype, flags, plen in msgs:
        conn = tuple(sorted([src, dst]))
        if mtype == 1:
            pending_q[conn] = t
        elif mtype == 2:
            if conn in pending_q:
                q_lat.append((t - pending_q.pop(conn)) * 1000.0)
            if flags & 0x01:
                hits += 1
            elif flags & 0x02:
                misses += 1
        elif mtype == 3:
            pending_a[conn] = t
        elif mtype == 4 and conn in pending_a:
            a_lat.append((t - pending_a.pop(conn)) * 1000.0)

    rtts = []
    txt = tshark(["-r", path, "-Y", f"tcp.port=={port} && tcp.analysis.ack_rtt",
                  "-T", "fields", "-e", "tcp.analysis.ack_rtt"])
    for line in txt.splitlines():
        try:
            rtts.append(float(line) * 1000.0)
        except ValueError:
            pass

    conns = count(path, f"tcp.flags.syn==1 && tcp.flags.ack==0 && tcp.dstport=={port}")
    return {
        "file": os.path.basename(path),
        "window_s": round(window, 2),
        "messages": len(msgs),
        "connections": conns,
        "app_bytes": app_bytes,
        "throughput_Bps": round(app_bytes / window, 1) if window else 0,
        "question_n": len(q_lat),
        "question_mean_ms": round(st.mean(q_lat), 1) if q_lat else 0,
        "question_p95_ms": round(sorted(q_lat)[int(.95 * (len(q_lat) - 1))], 1) if q_lat else 0,
        "answer_n": len(a_lat),
        "answer_mean_ms": round(st.mean(a_lat), 2) if a_lat else 0,
        "cache_hits": hits,
        "cache_misses": misses,
        "retransmissions": count(path, "tcp.analysis.retransmission"),
        "dup_acks": count(path, "tcp.analysis.duplicate_ack"),
        "zero_windows": count(path, "tcp.analysis.zero_window"),
        "ack_rtt_mean_ms": round(st.mean(rtts), 3) if rtts else 0,
    }


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("pcaps", nargs="+")
    ap.add_argument("--port", type=int, default=11047)
    ap.add_argument("--csv", help="also write the table to this file")
    args = ap.parse_args()

    if not shutil.which("tshark"):
        sys.exit("tshark not found: sudo apt install tshark")

    rows = []
    for path in args.pcaps:
        row = analyse(path, args.port)
        if not row:
            print(f"{path}: no quiz messages on port {args.port}")
            continue
        rows.append(row)
        print(f"\n--- {row['file']} ---")
        for k, v in row.items():
            if k != "file":
                print(f"  {k:<20} {v}")

    if args.csv and rows:
        os.makedirs(os.path.dirname(os.path.abspath(args.csv)), exist_ok=True)
        with open(args.csv, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        print("\nwrote", args.csv)
