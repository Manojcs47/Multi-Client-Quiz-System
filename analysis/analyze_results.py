#!/usr/bin/env python3
"""
Turns the per-client CSVs written by run_sweep.py into the tables and the
two graphs the report asks for  (cs23btech11047).

    python3 analysis/analyze_results.py                # both parts
    python3 analysis/analyze_results.py --part B

Outputs
    results/summary_part<a|b>.csv     one row per client count
    results/summary.md               markdown tables to paste in the report
    results/latency_vs_clients.png   question and answer latency vs N
    results/throughput_vs_clients.png

matplotlib is optional; without it the numbers are still produced.
"""
import argparse
import csv
import glob
import os
import statistics as st

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")


def pct(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    k = min(len(values) - 1, int(round((p / 100.0) * (len(values) - 1))))
    return values[k]


def load_run(run_dir):
    """Return (question_latencies, answer_latencies, hits, misses, throughputs,
    total_bytes, clients)."""
    q, a, lb, hit, miss, tput, total, dur = [], [], [], 0, 0, [], 0, []
    for path in sorted(glob.glob(os.path.join(run_dir, "*.csv"))):
        with open(path, newline="") as fh:
            for row in csv.DictReader(fh):
                ev = row["event"]
                if ev == "question":
                    q.append(float(row["latency_ms"]))
                    if row["cache"] == "hit":
                        hit += 1
                    elif row["cache"] == "miss":
                        miss += 1
                elif ev == "answer":
                    a.append(float(row["latency_ms"]))
                elif ev == "leaderboard":
                    lb.append(float(row["latency_ms"]))
                elif ev == "session":
                    tput.append(float(row["throughput_Bps"]))
                    total += int(row["bytes_sent"]) + int(row["bytes_recv"])
                    dur.append(float(row["duration_s"]))
    return dict(q=q, a=a, lb=lb, hit=hit, miss=miss, tput=tput,
                total=total, dur=dur)


def summarise(part):
    base = os.path.join(RESULTS, "part" + part)
    if not os.path.isdir(base):
        return []
    rows = []
    for run_dir in sorted(glob.glob(os.path.join(base, "N*")),
                          key=lambda p: int(os.path.basename(p)[1:])):
        n = int(os.path.basename(run_dir)[1:])
        d = load_run(run_dir)
        if not d["q"] and not d["a"]:
            continue
        cpu = ""
        cpu_file = os.path.join(run_dir, "server_cpu.txt")
        if os.path.exists(cpu_file):
            with open(cpu_file) as fh:
                cpu = fh.readline().strip().split(",")[-1]
        wall = max(d["dur"]) if d["dur"] else 0
        rows.append({
            "clients": n,
            "questions": len(d["q"]),
            "q_mean_ms": round(st.mean(d["q"]), 1) if d["q"] else 0,
            "q_median_ms": round(st.median(d["q"]), 1) if d["q"] else 0,
            "q_p95_ms": round(pct(d["q"], 95), 1),
            "ans_mean_ms": round(st.mean(d["a"]), 2) if d["a"] else 0,
            "ans_p95_ms": round(pct(d["a"], 95), 2),
            "lb_mean_ms": round(st.mean(d["lb"]), 2) if d["lb"] else 0,
            "cache_hits": d["hit"],
            "cache_misses": d["miss"],
            "hit_rate_%": round(100.0 * d["hit"] / max(1, d["hit"] + d["miss"]), 1),
            "per_client_Bps": round(st.mean(d["tput"]), 1) if d["tput"] else 0,
            "aggregate_Bps": round(d["total"] / wall, 1) if wall else 0,
            "server_cpu_%": cpu,
        })
    return rows


def write_csv(part, rows):
    if not rows:
        return
    path = os.path.join(RESULTS, f"summary_part{part}.csv")
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print("wrote", path)


def markdown(part, rows):
    if not rows:
        return ""
    head = ["clients", "questions", "q_mean_ms", "q_median_ms", "q_p95_ms",
            "ans_mean_ms", "hit_rate_%", "per_client_Bps", "aggregate_Bps",
            "server_cpu_%"]
    out = [f"\n### Part {part.upper()}\n",
           "| " + " | ".join(head) + " |",
           "|" + "---|" * len(head)]
    for r in rows:
        out.append("| " + " | ".join(str(r[h]) for h in head) + " |")
    return "\n".join(out) + "\n"


def plot(all_rows):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed - skipping the graphs "
              "(pip install --user matplotlib)")
        return

    fig, ax = plt.subplots(figsize=(7, 4.2))
    for part, rows in all_rows.items():
        if not rows:
            continue
        xs = [r["clients"] for r in rows]
        ax.plot(xs, [r["q_mean_ms"] for r in rows], marker="o",
                label=f"Part {part.upper()} question latency (mean)")
        ax.plot(xs, [r["ans_mean_ms"] for r in rows], marker="s", linestyle="--",
                label=f"Part {part.upper()} answer latency (mean)")
    ax.set_xlabel("concurrent clients")
    ax.set_ylabel("latency (ms)")
    ax.set_yscale("log")
    ax.set_title("Latency vs number of concurrent clients")
    ax.grid(alpha=.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    p1 = os.path.join(RESULTS, "latency_vs_clients.png")
    fig.savefig(p1, dpi=160)
    print("wrote", p1)

    fig, ax = plt.subplots(figsize=(7, 4.2))
    for part, rows in all_rows.items():
        if not rows:
            continue
        xs = [r["clients"] for r in rows]
        ax.plot(xs, [r["aggregate_Bps"] for r in rows], marker="o",
                label=f"Part {part.upper()} aggregate")
        ax.plot(xs, [r["per_client_Bps"] for r in rows], marker="s", linestyle="--",
                label=f"Part {part.upper()} per client")
    ax.set_xlabel("concurrent clients")
    ax.set_ylabel("application throughput (bytes/s)")
    ax.set_title("Throughput vs number of concurrent clients")
    ax.grid(alpha=.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    p2 = os.path.join(RESULTS, "throughput_vs_clients.png")
    fig.savefig(p2, dpi=160)
    print("wrote", p2)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--part", choices=["A", "B", "a", "b"], help="default: both")
    args = ap.parse_args()
    parts = [args.part.lower()] if args.part else ["a", "b"]

    os.makedirs(RESULTS, exist_ok=True)
    all_rows, md = {}, "# Performance summary (cs23btech11047)\n"
    for p in parts:
        rows = summarise(p)
        all_rows[p] = rows
        write_csv(p, rows)
        md += markdown(p, rows)
        if not rows:
            print(f"no results found for part {p.upper()} "
                  f"(expected results/part{p}/N*/)")

    with open(os.path.join(RESULTS, "summary.md"), "w") as fh:
        fh.write(md)
    print("wrote", os.path.join(RESULTS, "summary.md"))
    print(md)
    plot(all_rows)
