# CS3530 Assignment-1 — Multi-Client Quiz System with LLM Integration

Roll number **cs23btech11047** · quiz server port **11047** · LLM backend
**192.168.50.142:25000**

```
.
├── PARTA/            1(A) client–server, no cache
├── PARTB/            1(B) client–server, with cache
├── frontend/         browser UI (bridge.py + index.html)
├── analysis/         Part 2: sweep driver, pcap analysis, plots, LLM probe
├── tools/            mock LLM, python load-test client
├── PROTOCOL.md       protocol design document
├── REPORT.md         report skeleton with the measurement methodology
└── README.md
```

---

## 1. Setup

**Tested on** Ubuntu 22.04 / 24.04, `g++ 11.4` and `13.3`, C++11, glibc 2.35+.

Dependencies — two, both usually already present on the lab server:

```bash
sudo apt install g++ make libcurl4-openssl-dev          # build
sudo apt install tcpdump tshark                         # Part 2 captures
pip3 install --user matplotlib                          # optional, for the graphs
```

`json.hpp` (nlohmann/json 3.11.3) ships inside `PARTA/` and `PARTB/`. If it is
ever missing, `make json` downloads it.

Nothing else is needed: the frontend bridge and every analysis script use only
the Python 3 standard library (matplotlib excepted, and it degrades gracefully).

### Build

```bash
cd PARTA && make          # produces ./server and ./client
cd ../PARTB && make
```

`make clean` removes the binaries. The Makefile also searches
`$HOME/local/include` and `$HOME/local/lib`, so a locally built libcurl is
picked up automatically.

---

## 2. Running Part 1(A) — without cache

**Terminal 1 (on 192.168.50.111):**

```bash
cd PARTA
./server                      # listens on 11047, reserves the LLM first
./server --port 11047         # the port is overridable if 11047 is busy
```

**Terminals 2…N (client host / laptop):**

```bash
cd PARTA
./client --host 192.168.50.111 --port 11047
```

The client asks for a username and a genre, then drops into the quiz.

### Client commands

| Key | Action |
|---|---|
| `A` `B` `C` `D` | submit that option as your answer |
| `N` | next question in the same genre |
| `G <genre>` | switch genre and fetch a question |
| `L` | request and print the leaderboard |
| `Q` | end the session and print the final scores |

### Defaults (all hardcoded in the sources, all visible in one place)

| Setting | Value | Where |
|---|---|---|
| Listen port | 11047 | `SERVER_PORT` in `server.cpp` |
| Max concurrent clients | 10 | `MAX_CLIENTS` |
| Session length | 300 s (5 min) | `SESSION_SECONDS` |
| Keep-alive interval / deadline | 5 s / 5 s | `KEEPALIVE_SECONDS` |
| LLM retries per request | 3 | `LLM_RETRIES` |
| Questions per LLM call | 1 (Part A), 10 (Part B) | `QUESTIONS_PER_FETCH` |
| Cache TTL | 15 min (Part B) | `CACHE_TTL_MINUTES` |
| Cached genres kept | 32 (Part B, LRU) | `MAX_CACHED_GENRES` |

---

## 3. Running Part 1(B) — with cache

Identical commands, from the `PARTB` directory:

```bash
cd PARTB && ./server
cd PARTB && ./client --host 192.168.50.111 --port 11047
```

The client prints `[served from cache]` or `[freshly generated]` above each
question, driven by the header flag rather than by the JSON body.

### Automated clients (required for 1(B) testing)

```bash
# one self-driving bot: random answers, random genre if --genre is omitted
./client --host 192.168.50.111 --auto --user Bot_1 --duration 60

# ten of them at once
for i in $(seq 1 10); do
  ./client --host 192.168.50.111 --auto --user Bot_$i --genre Science \
           --duration 30 --metrics /tmp/Bot_$i.csv --clients 10 --quiet &
done; wait
```

Full option list: `./client --help`.

---

## 4. Sample run

**Server (Part B, 3 clients):**

```
Reserving the LLM instance for cs23btech11047 ...
LLM reserved.
[17:04:46.194] Part B server listening on port 11047 (cache on, TTL 15 min, batch 10, max 10 clients)
[17:04:48.190] client 192.168.50.23:47978 connected. Active clients: 1
[17:04:48.190] client 192.168.50.23:47990 connected. Active clients: 2
[17:04:48.190] client 192.168.50.23:47990 registered as 'Bot_2'. Session started.
[17:04:48.190] GENRE_REQUEST from 'Bot_2' : science
[17:04:48.190] CACHE WAIT  genre='science' client='Bot_3' - a fetch is already in flight
[17:04:49.192] LLM returned 10 question(s) for genre='science' in 1001 ms
[17:04:49.192] CACHE FILL  genre='science' +10 new question(s), pool=10
[17:04:49.192] CACHE MISS  genre='science' client='Bot_2' -> LLM called, pool now 10 question(s)
[17:04:49.192] CACHE HIT   genre='science' client='Bot_1' pool=10 age=0s (no LLM call)
[17:04:49.192] ANSWER 'D' from 'Bot_1' -> wrong
[17:04:58.435] KEEP_ALIVE -> 'Bot_1'
[17:04:58.436] ALIVE_OK <- 'Bot_1'
[17:05:11.321] removed 'Bot_1' (final score 2, reason client_quit); leaderboard pushed to 2 client(s)
```

**Client:**

```
 Connected to 192.168.50.111:11047
 Connected as 'Ananya'  |  session 300s  |  cache on

 Question 1  [freshly generated]
 Which planet has the shortest day?
   A) Mercury
   B) Jupiter
   C) Mars
   D) Venus
 Answer with A, B, C or D.
 (question latency 1001 ms)
> C

 Wrong  (correct answer: B)   score 0   (result latency 1 ms)
> L

 Leaderboard
  1. Bot_3   4 / 6
  2. Ananya  0 / 1
> Q
 Ending the session.

 Quiz over - final scores available
 Your score: 0 correct out of 1
 Session ended because: client_quit
 Final leaderboard
  1. Bot_3   4
  2. Ananya  0

[Ananya] session 19 s | sent 97 B | received 645 B | application throughput 39 B/s | score 0
```

Every client that disconnects triggers an unsolicited leaderboard push to
everyone still playing, so the other terminals print an updated table without
being asked.

---

## 5. Web frontend

A browser cannot open a raw TCP socket, so `frontend/bridge.py` holds the TCP
connection and relays it to the page over Server-Sent Events. It answers
`KEEP_ALIVE` on the page's behalf and forwards every frame — including the
cache flag — so the UI can show the protocol live.

```bash
# terminal 1 — the quiz server (PARTA or PARTB)
cd PARTB && ./server

# terminal 2 — the bridge, on the client host
python3 frontend/bridge.py --server-host 192.168.50.111 --server-port 11047 --listen 8080

# browser
http://localhost:8080
```

To let other machines on the LAN use the UI, add `--bind 0.0.0.0`.

The page shows the question and options, a 5-minute countdown, the live
leaderboard, the measured question latency, and an "on the wire" panel listing
every frame with its direction, type, cache flag and byte count. Keyboard:
`A`–`D` to answer, `N` for the next question, `L` for the leaderboard.

Bridge options: `python3 frontend/bridge.py --help`.

---

## 6. Part 2 — captures and measurements

### 6.1 Capture commands

```bash
# single client, captured at the client host NIC (Fig 1)
sudo tcpdump -i eth0 -s 0 -w pcaps/parta_single_client.pcap 'port 11047'

# multiple clients, captured at the server interface facing the clients (Fig 2 b-i)
sudo tcpdump -i eth0 -s 0 -w pcaps/partb_N10_serverside.pcap 'port 11047'
```

Wrapper with a built-in 30-second limit:

```bash
sudo ./analysis/capture.sh eth0 partb_N10_serverside 30
```

Use `-i lo` when the clients and the server run on the same host. Every capture
is filtered to the quiz port and kept to 30 s, as the assignment requires.

### 6.2 Concurrency sweep

```bash
# Part A: 1,3,5,7,10 clients, 30 s each, capture a trace per run
sudo python3 analysis/run_sweep.py --part A --host 192.168.50.111 \
     --clients 1 3 5 7 10 --duration 30 --pcap --iface eth0

# Part B, same sweep; add --server-pid <pid> to sample server CPU
python3 analysis/run_sweep.py --part B --host 192.168.50.111 \
     --clients 1 2 4 6 8 10 --duration 30 --server-pid $(pgrep -f 'PARTB/server')

# stress the cache with a different genre per client
python3 analysis/run_sweep.py --part B --clients 10 --duration 30 --mixed-genres
```

Output: `results/part<a|b>/N<k>/Bot_*.csv`, one row per question, per answer
and per leaderboard request, plus a final `session` row with the byte counts.

### 6.3 Tables and graphs

```bash
python3 analysis/analyze_results.py
```

Writes `results/summary_parta.csv`, `results/summary_partb.csv`,
`results/summary.md` (markdown tables ready to paste into the report) and, if
matplotlib is installed, `results/latency_vs_clients.png` and
`results/throughput_vs_clients.png`.

### 6.4 Trace analysis

```bash
python3 analysis/analyze_pcap.py pcaps/*.pcap --port 11047 --csv results/pcap_summary.csv
```

This reassembles the quiz protocol out of `tcp.payload`, so the latency numbers
come from the packets themselves rather than from the client's own clock. It
also reports retransmissions, duplicate ACKs, zero-window events and mean ACK
RTT — the TCP-level evidence for the bottleneck section.

Useful Wireshark display filters:

| Goal | Filter |
|---|---|
| Quiz traffic only | `tcp.port == 11047` |
| Only protocol messages | `tcp.port == 11047 && tcp.len > 0` |
| Question deliveries | `tcp.payload[4:4] == 00:00:00:02` |
| Cache hits only | `tcp.payload[3:1] == 01` |
| Cache misses only | `tcp.payload[3:1] == 02` |
| Keep-alive probes | `tcp.payload[4:4] == 00:00:00:08` |
| Trouble | `tcp.analysis.flags` |

### 6.5 LLM threshold

```bash
# run this on 192.168.50.111 — the LLM is only reachable from there
python3 analysis/llm_probe.py --concurrency 1 2 4 8 --requests 8
```

The requests-per-second column plateaus once the backend saturates; that
plateau is the LLM threshold quoted in the report.

---

## 7. Testing without the LLM

When the shared instance is busy, reserved by someone else, or you are working
off-campus, run the stub backend and point the server at it. Nothing else
changes, so the cache comparison and the concurrency sweep still work.

```bash
# terminal 1
python3 tools/mock_llm.py --port 25000 --delay 1.5

# terminal 2
QUIZ_LLM_HOST=127.0.0.1 QUIZ_LLM_PORT=25000 ./server
```

`QUIZ_LLM_HOST`, `QUIZ_LLM_PORT` and `QUIZ_ROLLNO` override the compiled-in
defaults; without them the server talks to `192.168.50.142:25000` as
`cs23btech11047`.

A Python load-test client is also available if the C++ one has not been built:

```bash
python3 tools/testing.py --clients 5 --duration 60 --host 192.168.50.111
```

---

## 8. Notes on behaviour worth knowing before the viva

- The server **reserves the LLM before it binds the socket** and refuses to
  start if the reservation fails, so a broken backend can never look like a
  networking bug. A background thread renews the reservation every 2 minutes,
  because the slot is voided after 5 minutes of idle time — which is easy to
  hit in Part B once the cache is warm.
- Clients 11 and beyond receive a `SESSION_END` carrying
  `{"error": "Server is full …"}` and are then closed; they are never counted
  in `active_clients`.
- Duplicate usernames are made unique (`Ananya`, `Ananya#2`) instead of
  silently overwriting each other's socket entry.
- Answers are normalised before comparison, so `c`, ` C `, and
  `C) George Washington` all match the letter `C`.
- The server ignores `SIGPIPE` and sends with `MSG_NOSIGNAL`, so a client
  disappearing mid-write cannot kill it.
