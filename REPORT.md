# Report — Multi-Client Quiz System with LLM Integration

**CS3530 Assignment-1** · Roll number `cs23btech11047` · Server port `11047`

> **How to use this file.** Sections 1, 3 and 5 are complete. Sections 2, 4 and
> 6 contain the structure plus the exact commands that produce each number;
> fill in the tables from `results/summary.md` and `results/pcap_summary.csv`
> after running the sweep on the lab machines, and drop the screenshots into
> `report_assets/`. Every number marked **TODO** must come from your own run —
> the placeholder values shown are from a local run against the stub backend in
> `tools/mock_llm.py` and are there only to show the expected shape of the data.

---

## 1. Protocol design

The full design document is in [`PROTOCOL.md`](PROTOCOL.md). Summary:

- 12-byte fixed header — `magic(2) | version(1) | flags(1) | type(4) | length(4)`,
  all integers big-endian — followed by a UTF-8 payload, almost always JSON.
- Twelve message types covering questions, answers, results, leaderboards,
  liveness and session control.
- Part B extends Part A through the **flags byte** rather than by adding new
  message types: `0x01 = CACHE_HIT`, `0x02 = CACHE_MISS` on `QUESTION_DELIVERY`.
  A Part A client ignores the flag and still works against a Part B server.
- Receivers loop until a whole frame has arrived, cap `length` at 1 MiB, and
  validate the magic and version before trusting anything else.

Example exchanges, including the exact bytes of a join message and the JSON
bodies of each message type, are in §5 of `PROTOCOL.md`.

---

## 2. Testing evidence

### 2.1 Part 1(A) — three or more concurrent clients

*Screenshots to include:* `report_assets/parta_server.png`,
`report_assets/parta_client1.png`, `report_assets/parta_client2.png`,
`report_assets/parta_client3.png`, `report_assets/parta_leaderboard.png`.

Reproduce with:

```bash
cd PARTA && ./server                                    # terminal 1
cd PARTA && ./client --host 192.168.50.111              # terminals 2,3,4
```

What the screenshots must show:
1. three clients connected at once (server log lines with distinct
   `ip:port` and usernames, `Active clients: 3`),
2. each client receiving a question and submitting an answer,
3. the server scoring the answers (`ANSWER 'C' from 'X' -> correct`),
4. a leaderboard printed on a client after pressing `L`,
5. `Quiz over - final scores available` on session end.

Also worth capturing, because both are explicit requirements:
- the keep-alive exchange (`KEEP_ALIVE -> 'X'` followed by `ALIVE_OK <- 'X'`),
- an unresponsive client being dropped. Reproduce it with a client that never
  replies:

```
[17:06:26.685] client 127.0.0.1:36558 registered as 'Zombie'. Session started.
[17:06:31.715] KEEP_ALIVE -> 'Zombie'
[17:06:36.835] client 'Zombie' did not answer KEEP_ALIVE within 5s - dropping
[17:06:36.835] removed 'Zombie' (final score 0, reason keepalive_timeout); leaderboard pushed to 0 client(s)
```

The probe goes out after 5 s of silence and the client is removed 5 s later,
exactly as specified.

### 2.2 Part 1(B) — the same set, with cache evidence

*Screenshots to include:* `report_assets/partb_*.png`.

The extra thing to show here is the cache. A cold genre followed by warm
requests produces this pattern on the server (real output from a three-bot run):

```
[17:04:48.190] GENRE_REQUEST from 'Bot_2' : science
[17:04:48.190] CACHE WAIT  genre='science' client='Bot_3' - a fetch is already in flight
[17:04:48.191] CACHE WAIT  genre='science' client='Bot_1' - a fetch is already in flight
[17:04:49.192] LLM returned 10 question(s) for genre='science' in 1001 ms
[17:04:49.192] CACHE FILL  genre='science' +10 new question(s), pool=10
[17:04:49.192] CACHE MISS  genre='science' client='Bot_2' -> LLM called, pool now 10 question(s)
[17:04:49.192] CACHE HIT   genre='science' client='Bot_1' pool=10 age=0s (no LLM call)
[17:04:49.192] CACHE HIT   genre='science' client='Bot_3' pool=10 age=0s (no LLM call)
[17:04:49.993] CACHE HIT   genre='science' client='Bot_2' pool=10 age=0s (no LLM call)
```

and on the client side:

```
[Bot_1] question in 1001 ms (cache)
[Bot_1] question in 0 ms (cache)
[Bot_1] question in 0 ms (cache)
```

The corresponding headline comparison, same workload (3 bots, 20 s, backend
with a 1 s generation delay):

| | LLM calls | first question | later questions |
|---|---|---|---|
| Part A (no cache) | 22 | 1001 ms | ~1001 ms every time |
| Part B (cache) | 1 | 1001 ms | 0.07–0.26 ms |

**TODO:** repeat against the real LLM and replace the table.

---

## 3. Cache design and invalidation policy (Part 1B)

### 3.1 Structure

`std::map<std::string, CacheEntry>` keyed by the lower-cased genre. Each entry
holds a vector of validated `QuizQuestion` (question, four options, answer
letter), the time the pool was filled, the time it was last touched, and
counters. One `pthread_mutex_t` guards the map; no LLM call is ever made while
holding it.

One LLM call fetches **10** questions rather than one. That single decision is
what turns the cache from a marginal optimisation into an order-of-magnitude
one: a genre costs one round trip and then serves ten questions to every client
that asks for it.

Each client thread keeps its own `std::set` of question texts it has already
been served, so two clients on the same genre get different questions out of
the same pool while the pool itself is shared.

### 3.2 Invalidation — three rules

1. **Time to live — 15 minutes.** Checked on every access; an older entry is
   dropped and the next request refills it. This bounds staleness and stops a
   long-running server from serving questions generated hours earlier. Fifteen
   minutes was chosen against the 5-minute session length: it is long enough
   that a whole session is normally served from one fetch, short enough that a
   second cohort of players sees fresh material.
2. **Exhaustion refill.** When a client has already seen every question in the
   pool, the entry is refilled from the LLM and the new questions are merged in.
   Correctness rule: nobody is served the same question twice inside a session.
   After two refills that fail to produce anything new, the client's seen-set is
   cleared and questions repeat, rather than failing the request.
3. **LRU capacity — 32 genres.** Entries are evicted least-recently-used first,
   which bounds memory at roughly 32 × 10 questions regardless of how many
   distinct genres clients invent. A genre that is currently being fetched is
   never chosen as the victim.

### 3.3 Stampede protection

A cold genre requested by ten clients simultaneously would otherwise produce ten
identical LLM requests. A `std::set<std::string> fetch_in_progress` plus a
condition variable means the first thread fetches while the rest wait on
`pthread_cond_wait`; when the fetch lands they are broadcast awake and served
from the now-warm pool. The `CACHE WAIT` lines in §2.2 are this mechanism
working. Consequence for the numbers: the first client of a cohort records a
cache **miss**, every other client records a **hit** at the same latency,
and the LLM sees exactly one request.

### 3.4 Why the flag and not a new message type

Cache status is a property of one particular `QUESTION_DELIVERY`, not a
different kind of message. Putting it in the header's flags byte keeps the
message-type space clean, lets a Part A client stay compatible, and — usefully
for Part 2 — makes hit/miss readable straight out of a pcap with the filter
`tcp.payload[3:1] == 01`, without JSON parsing.

---

## 4. Performance measurement and traffic analysis

### 4.1 Metric definitions

| Metric | Definition | Measured where |
|---|---|---|
| **Question latency** | Time from the client sending `GENRE_REQUEST` (type 1) to receiving the matching `QUESTION_DELIVERY` (type 2) on the same connection | Client (`--metrics` CSV) and independently from the pcap (`analyze_pcap.py`) |
| **Answer latency** | Time from `ANSWER_SUBMISSION` (type 3) to `RESULT_DELIVERY` (type 4) | Same |
| **Leaderboard latency** | `LEADERBOARD_REQUEST` (5) to `LEADERBOARD_DELIVERY` (6) | Same |
| **Per-client throughput** | (bytes sent + bytes received, header + payload) ÷ session seconds | Client `session` CSV row |
| **Aggregate throughput** | Sum over all clients in a run ÷ run wall time | `analyze_results.py` |
| **Trace throughput** | Sum of `12 + length` over every protocol message in the capture ÷ capture window | `analyze_pcap.py` |
| **Server CPU** | `utime + stime` delta from `/proc/<pid>/stat` over the run ÷ wall time | `run_sweep.py --server-pid` |
| **Timeouts / loss** | `tcp.analysis.retransmission`, `duplicate_ack`, `zero_window` counts | `analyze_pcap.py` |

Client-side and packet-side latency are measured independently and should agree
to within a millisecond; where they differ, the pcap number is the one quoted,
since the assignment asks for pcap-justified answers.

### 4.2 Method

Captures are filtered to the quiz port and capped at 30 s per run, one trace
file per run:

```bash
sudo tcpdump -i eth0 -s 0 -w pcaps/partb_N10.pcap 'port 11047'
```

Concurrency sweep, N ∈ {1, 3, 5, 7, 10} (and 1, 2, 4, 6, 8, 10 for the graphs
the assignment asks for):

```bash
# 1. start the server
cd PARTB && ./server &

# 2. sweep, capturing a trace per run
sudo python3 analysis/run_sweep.py --part B --host 192.168.50.111 \
     --clients 1 3 5 7 10 --duration 30 --pcap --iface eth0 \
     --server-pid $(pgrep -f 'PARTB/server')

# 3. tables and graphs
python3 analysis/analyze_results.py

# 4. packet-level numbers
python3 analysis/analyze_pcap.py pcaps/*.pcap --port 11047 --csv results/pcap_summary.csv
```

Each bot answers after a uniform 0.5–2.0 s think time and requests the next
question after a further 1–3 s, which keeps the offered load realistic instead
of being a tight loop.

### 4.3 Results — Part 1(A)

Paste from `results/summary_parta.csv`.

| clients | questions | q mean (ms) | q p95 (ms) | answer mean (ms) | per-client B/s | aggregate B/s | server CPU % |
|---|---|---|---|---|---|---|---|
| 1 | TODO | TODO | TODO | TODO | TODO | TODO | TODO |
| 3 | | | | | | | |
| 5 | | | | | | | |
| 7 | | | | | | | |
| 10 | | | | | | | |

### 4.4 Results — Part 1(B)

Paste from `results/summary_partb.csv`; the extra columns that matter are
`cache_hits`, `cache_misses` and `hit_rate_%`.

| clients | questions | q mean (ms) | q p95 (ms) | hit rate % | answer mean (ms) | aggregate B/s | server CPU % |
|---|---|---|---|---|---|---|---|
| 1 | TODO | | | | | | |
| 3 | | | | | | | |
| 5 | | | | | | | |
| 7 | | | | | | | |
| 10 | | | | | | | |

### 4.5 Graphs

- `results/latency_vs_clients.png` — question and answer latency against N, both
  parts on one log-scale axis. Expect Part A to sit near the LLM service time at
  every N and Part B to sit near the network RTT after the first request.
- `results/throughput_vs_clients.png` — aggregate and per-client application
  throughput against N. Aggregate should rise roughly linearly while the server
  is not saturated; per-client should stay flat until it is.

### 4.6 Packet-level cross-check

From `results/pcap_summary.csv`:

| trace | messages | window s | throughput B/s | q mean ms | retrans | dup ACK | zero win | ACK RTT ms |
|---|---|---|---|---|---|---|---|---|
| parta_N10.pcap | TODO | | | | | | | |
| partb_N10.pcap | TODO | | | | | | | |

---

## 5. Bottleneck identification

The three candidates are the network, the server CPU, and the LLM. They are
separated as follows, each with the evidence that decides it.

**Network.** Quiz messages are small: a question is ~230 B on the wire, an
answer 13 B, a leaderboard ~200 B. At 10 clients the aggregate is on the order
of a few kB/s — orders of magnitude below a 1 Gb/s LAN. The pcap confirms it:
if `retransmissions`, `duplicate_ack` and `zero_window` are all ~0 and mean ACK
RTT stays well under a millisecond as N grows, the network is not the limit.
Quote those four columns from §4.6.

**Server CPU.** The server does JSON serialisation, a string comparison and a
map lookup per message; the only real work is `curl_easy_perform`, which blocks
a thread without burning CPU. Expect the `server_cpu_%` column to stay in single
digits at N = 10. If it does, the server is not the limit either. The thread
count is also bounded — one detached thread per client, capped at 10 — so there
is no thread-explosion effect to measure.

**LLM.** This is the bottleneck in Part A, and the numbers show it three ways:

1. Question latency in Part A tracks the LLM service time almost exactly, while
   answer latency — which never touches the LLM — stays in the sub-millisecond
   range. A single system serving both cannot have a network or CPU problem that
   affects only one of them.
2. Part A issues one LLM request per question. In the three-bot run it made 22
   calls in 20 s; Part B made 1 for the same workload. Latency per question fell
   from ~1000 ms to ~0.2 ms.
3. `analysis/llm_probe.py` measures the backend directly. Fill in:

| concurrency | ok | failed | mean ms | p95 ms | req/s |
|---|---|---|---|---|---|
| 1 | TODO | | | | |
| 2 | | | | | |
| 4 | | | | | |
| 8 | | | | | |

**TODO — answer 4(a):** the req/s column plateaus at **____ requests/second**;
that is the LLM threshold. Note also that the instance is shared by up to 24
students, so the figure you measure is the share available at that moment, not
the hardware limit.

**Conclusion to write once the numbers are in:** in Part A the system is
LLM-bound — N concurrent clients produce N concurrent generate_quiz requests,
and once N exceeds the LLM's concurrency the queue at the backend, not the
server or the network, sets the latency. In Part B the cache removes almost all
of those requests, and the system becomes bound by the client think time, with
latency at the network RTT.

---

## 6. Server concurrency limit

**TODO — answer 4(b).** The hard limit is the configured `MAX_CLIENTS = 10`;
client 11 receives `SESSION_END` with `{"error": "Server is full …"}` and is
closed, which is the designed behaviour rather than a failure. The question to
answer with evidence is where latency becomes unacceptable *below* that cap.

Procedure, with `MAX_CLIENTS` raised temporarily to probe past 10:

```bash
python3 analysis/run_sweep.py --part A --clients 1 2 4 6 8 10 12 16 20 --duration 30
python3 analysis/run_sweep.py --part B --clients 1 2 4 6 8 10 12 16 20 --duration 30
python3 analysis/analyze_results.py
```

Report, for each part, the largest N at which:

- no client logs a socket timeout or a `keepalive_timeout` disconnect,
- p95 question latency stays within your stated acceptability bound (a
  reasonable one to defend: 2× the single-client p95),
- the pcap shows no retransmissions or zero-window events.

Expected shape of the answer: Part A degrades first and degrades at the LLM, not
at the server — the curve bends where LLM concurrency saturates. Part B should
hold flat to the 10-client cap and well beyond it, because after the first fetch
almost every request is a memory lookup.

---

## 7. Deliverables checklist

- [x] Source code, Part 1(A) — `PARTA/` (`server.cpp`, `client.cpp`, `protocol.h`, `llm.h`, `Makefile`)
- [x] Source code, Part 1(B) — `PARTB/`
- [x] README with compile and run steps, sample run, expected output — `README.md`
- [x] Protocol design document — `PROTOCOL.md`
- [ ] `.pcap` files for all test scenarios — `pcaps/` (**run the captures**)
- [ ] Screenshots / console logs for 3+ concurrent clients, both parts — `report_assets/`
- [ ] Performance analysis: tables, graphs, bottleneck findings — §4, §5, §6
- [x] Extra: web frontend (`frontend/`), stub LLM and Python bot (`tools/`),
      measurement tooling (`analysis/`)
