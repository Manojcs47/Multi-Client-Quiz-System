#!/usr/bin/env python3
"""
Concurrency sweep for CS3530 Assignment-1 Part 2  (cs23btech11047)

Starts N automated clients at once, lets them play for a fixed window and
collects one CSV per client. Optionally records a .pcap of the quiz port
while the run is in progress and samples the server process CPU.

    # Part A, capture traces, sweep 1,3,5,7,10 clients, 30 s each
    sudo python3 analysis/run_sweep.py --part A --iface lo --pcap \
         --clients 1 3 5 7 10 --duration 30

    # Part B without capture
    python3 analysis/run_sweep.py --part B --clients 1 2 4 6 8 10 --duration 30

Results land in  results/part<A|B>/N<k>/  and traces in  pcaps/.
Run analysis/analyze_results.py afterwards to build the tables and plots.
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import time

GENRES = ["Science", "History", "Maths", "Geography", "Movies",
          "Music", "Sports", "Literature", "Art", "Technology"]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_cpu(pid):
    """utime+stime in jiffies for a pid, or None."""
    try:
        with open(f"/proc/{pid}/stat") as fh:
            f = fh.read().split()
        return int(f[13]) + int(f[14])
    except (OSError, IndexError, ValueError):
        return None


def sweep(args):
    part_dir = os.path.join(ROOT, "PART" + args.part.upper())
    client = os.path.join(part_dir, "client")
    if not os.path.exists(client):
        sys.exit(f"{client} not found - run `make` in {part_dir} first")

    out_root = os.path.join(ROOT, "results", "part" + args.part.lower())
    pcap_dir = os.path.join(ROOT, "pcaps")
    os.makedirs(out_root, exist_ok=True)
    os.makedirs(pcap_dir, exist_ok=True)

    if args.pcap and not shutil.which("tcpdump"):
        sys.exit("tcpdump is not installed; drop --pcap or install it")

    for n in args.clients:
        run_dir = os.path.join(out_root, f"N{n}")
        os.makedirs(run_dir, exist_ok=True)
        print(f"\n=== {n} concurrent client(s), {args.duration}s ===")

        dumper = None
        pcap_path = os.path.join(pcap_dir, f"part{args.part.lower()}_N{n}.pcap")
        if args.pcap:
            cmd = ["tcpdump", "-i", args.iface, "-s", "0", "-w", pcap_path,
                   f"port {args.port}"]
            print("  capture:", " ".join(cmd))
            dumper = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL)
            time.sleep(1.5)          # let tcpdump attach before traffic starts

        cpu0 = read_cpu(args.server_pid) if args.server_pid else None
        wall0 = time.time()

        procs = []
        for i in range(n):
            name = f"Bot_{n}_{i + 1}"
            cmd = [client, "--host", args.host, "--port", str(args.port),
                   "--auto", "--user", name,
                   "--genre", GENRES[i % len(GENRES)] if args.mixed_genres
                              else args.genre,
                   "--duration", str(args.duration),
                   "--metrics", os.path.join(run_dir, name + ".csv"),
                   "--clients", str(n), "--quiet"]
            log = open(os.path.join(run_dir, name + ".log"), "w")
            procs.append((subprocess.Popen(cmd, stdout=log, stderr=log), log))

        for p, log in procs:
            p.wait()
            log.close()

        wall = time.time() - wall0
        if cpu0 is not None:
            cpu1 = read_cpu(args.server_pid)
            hz = os.sysconf("SC_CLK_TCK")
            pct = 100.0 * (cpu1 - cpu0) / hz / wall if cpu1 else 0.0
            with open(os.path.join(run_dir, "server_cpu.txt"), "w") as fh:
                fh.write(f"server_cpu_percent,{pct:.2f}\nwall_seconds,{wall:.2f}\n")
            print(f"  server CPU during run: {pct:.1f}%")

        if dumper:
            time.sleep(1.0)
            dumper.send_signal(signal.SIGINT)
            dumper.wait(timeout=10)
            size = os.path.getsize(pcap_path) if os.path.exists(pcap_path) else 0
            print(f"  trace: {pcap_path} ({size} bytes)")

        print(f"  metrics: {run_dir}")
        if args.gap and n != args.clients[-1]:
            print(f"  cooling down {args.gap}s")
            time.sleep(args.gap)

    print("\nSweep finished. Next: python3 analysis/analyze_results.py")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--part", choices=["A", "B", "a", "b"], required=True)
    ap.add_argument("--host", default="192.168.50.111")
    ap.add_argument("--port", type=int, default=11047)
    ap.add_argument("--clients", type=int, nargs="+", default=[1, 3, 5, 7, 10])
    ap.add_argument("--duration", type=int, default=30,
                    help="seconds per run (keep at 30 for the captured runs)")
    ap.add_argument("--genre", default="Science")
    ap.add_argument("--mixed-genres", action="store_true",
                    help="give every client a different genre (stresses the cache)")
    ap.add_argument("--pcap", action="store_true", help="record a trace per run")
    ap.add_argument("--iface", default="lo", help="interface for tcpdump")
    ap.add_argument("--server-pid", type=int, help="sample CPU of this pid")
    ap.add_argument("--gap", type=int, default=5, help="pause between runs")
    sweep(ap.parse_args())
