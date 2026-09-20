# Protocol design document

**CS3530 Assignment-1 — Multi-Client Quiz System with LLM Integration**
Roll number: `cs23btech11047` · Server port: `11047` · Transport: TCP

---

## 1. Design goals

The quiz needs to multiplex six different conversations (questions, answers,
results, leaderboards, liveness probes, session control) over one long-lived
TCP connection per client. TCP is a byte stream with no message boundaries, so
the protocol adds its own fixed-size header in front of every payload. The
header also has to carry one extra bit of information in Part B: whether the
question that follows came out of the server's cache or was just generated.

Three properties drove the design:

- **Self-delimiting.** Every message states its own length, so the receiver
  always knows where one message ends and the next begins.
- **Self-describing.** The type field tells the receiver what the payload
  means without any positional state machine.
- **Extensible without a version break.** A flags byte carries per-message
  metadata (cache hit / cache miss), so Part B extends Part A's protocol
  rather than replacing it. A Part A client and a Part B server interoperate;
  the Part A client simply ignores the flag.

---

## 2. Frame format

Every message on the wire is a 12-byte header followed by `length` bytes of
UTF-8 payload. All multi-byte integers are in network byte order (big-endian).

```
 byte   0        2      3      4                8               12
        +--------+------+------+----------------+----------------+
        | magic  | ver  | flags|      type      |     length     |
        +--------+------+------+----------------+----------------+
        |                  payload (length bytes, UTF-8)         |
        +---------------------------------------------------------+
```

| Field  | Size | Value | Purpose |
|---|---|---|---|
| `magic` | 2 | `0x515A` (`"QZ"`) | Sanity check; also lets the pcap parser resynchronise mid-stream |
| `version` | 1 | `0x01` | Protocol version |
| `flags` | 1 | bitmask | `0x01` CACHE_HIT, `0x02` CACHE_MISS, `0x00` not applicable |
| `type` | 4 | enum | Message type (table below) |
| `length` | 4 | 0 … 1 048 576 | Payload byte count; `0` is legal |

C++ (`protocol.h`):

```cpp
#pragma pack(push, 1)
struct MessageHeader {
    uint16_t magic;          // htons(0x515A)
    uint8_t  version;        // 1
    uint8_t  flags;          // FLAG_CACHE_HIT | FLAG_CACHE_MISS
    uint32_t type;           // htonl(Messagetype)
    uint32_t payload_length; // htonl(bytes that follow)
};
#pragma pack(pop)
```

Python equivalent (used by `bridge.py`, `tools/testing.py`,
`analysis/analyze_pcap.py`):

```python
struct.pack('!HBBII', 0x515A, 1, flags, msg_type, len(payload))
```

Payloads are JSON for everything except the two plain-string messages
(`USERNAME_SUBMISSION`, `GENRE_REQUEST`) and the single-letter
`ANSWER_SUBMISSION`. `length` is capped at 1 MiB on receive so a corrupt or
hostile header cannot make the peer allocate unbounded memory.

---

## 3. Message types

| # | Name | Direction | Payload | Notes |
|---|---|---|---|---|
| 1 | `GENRE_REQUEST` | C → S | genre string | Also means "next question" |
| 2 | `QUESTION_DELIVERY` | S → C | JSON question | Never contains the answer; carries the cache flag in Part B |
| 3 | `ANSWER_SUBMISSION` | C → S | `"A"`…`"D"` | Whitespace and case insensitive |
| 4 | `RESULT_DELIVERY` | S → C | JSON verdict | Includes the running score |
| 5 | `LEADERBOARD_REQUEST` | C → S | empty | Allowed at any time |
| 6 | `LEADERBOARD_DELIVERY` | S → C | JSON table | Sent on request and pushed on any disconnect |
| 7 | `SESSION_END` | both | JSON summary | Client sends it empty to quit; server replies with the summary |
| 8 | `KEEP_ALIVE` | S → C | empty | Sent after 5 s of silence |
| 9 | `ALIVE_OK` | C → S | empty | Must arrive within 5 s |
| 10 | `USERNAME_SUBMISSION` | C → S | name string | First message on the connection |
| 11 | `ERROR_MSG` | S → C | `{"error": …}` | Reserved for out-of-band errors |
| 12 | `SERVER_INFO` | S → C | JSON parameters | Session length, keep-alive interval, cache on/off |

### Flags (Part B)

| Flag | Value | Set on | Meaning |
|---|---|---|---|
| `FLAG_CACHE_HIT` | `0x01` | `QUESTION_DELIVERY` | Served from the cache, no LLM call was made |
| `FLAG_CACHE_MISS` | `0x02` | `QUESTION_DELIVERY` | This request triggered a fetch from the LLM |

Part A never sets either flag, so a client can distinguish "cache not in use"
from "cache used and missed". The same information is duplicated in the JSON
body as `"from_cache": true|false` for readability in logs and in the web UI.

---

## 4. Session lifecycle

```
client                                server                       LLM
  |---- TCP SYN --------------------->|
  |---- USERNAME_SUBMISSION "Ananya" ->|   register, log peer ip:port
  |<--- SERVER_INFO {300 s, cache on} -|
  |---- GENRE_REQUEST "Science" ------>|---- POST /generate_quiz -->|
  |                                    |<--- 10 questions ----------|
  |<--- QUESTION_DELIVERY [flags=02] --|   cache filled
  |---- ANSWER_SUBMISSION "C" -------->|   compare, score++
  |<--- RESULT_DELIVERY {correct} -----|
  |---- GENRE_REQUEST "Science" ------>|   (no LLM call)
  |<--- QUESTION_DELIVERY [flags=01] --|
  |        ... 5 s of silence ...      |
  |<--- KEEP_ALIVE --------------------|
  |---- ALIVE_OK --------------------->|
  |---- LEADERBOARD_REQUEST ---------->|
  |<--- LEADERBOARD_DELIVERY ----------|
  |---- SESSION_END ------------------>|
  |<--- SESSION_END {final summary} ---|
  |<--- TCP FIN -----------------------|
```

The session also ends when the 5-minute timer expires (the server sends
`SESSION_END` with `"reason": "time_limit"`), when a client fails to answer a
`KEEP_ALIVE` (`"reason": "keepalive_timeout"`), or when an eleventh client
connects (`"reason": "server_full"`, sent before the socket is closed).

---

## 5. Example exchanges

### 5.1 Joining

Client → server, 19 bytes total:

```
515a 01 00 0000000a 00000007   "Ananya"
 |    |  |     |         |
 |    |  |     |         +-- length = 7
 |    |  |     +------------ type = 10 (USERNAME_SUBMISSION)
 |    |  +------------------ flags = 0
 |    +--------------------- version = 1
 +-------------------------- magic  = 0x515A
```

Server → client (`SERVER_INFO`, type 12):

```json
{"username":"Ananya","session_seconds":300,"keepalive_seconds":5,
 "cache_enabled":true,"cache_ttl_minutes":15,"mode":"PART_B"}
```

### 5.2 Question on a cache miss (Part B, first client on a cold genre)

Client → server, type `1`, payload `Science`.
Server → client, type `2`, **flags `0x02`**:

```json
{"question_id":1,
 "q":"Which planet has the shortest day?",
 "question":"Which planet has the shortest day?",
 "options":["A) Mercury","B) Jupiter","C) Mars","D) Venus"],
 "genre":"Science","from_cache":false}
```

### 5.3 The same question pool on a cache hit

Server → client, type `2`, **flags `0x01`**, `"from_cache": true`. No HTTP
request to the LLM is made; the server log line reads:

```
[17:04:49.993] CACHE HIT   genre='science' client='Bot_1' pool=10 age=0s (no LLM call)
```

### 5.4 Answering

Client → server, type `3`, payload `C`.
Server → client, type `4`:

```json
{"verdict":"correct","message":"Correct","correct_option":"C","score":3}
```

or, when wrong:

```json
{"verdict":"wrong","message":"Wrong","correct_option":"A","score":2}
```

### 5.5 Leaderboard

Client → server, type `5`, empty payload (12 bytes on the wire).
Server → client, type `6`:

```json
{"entries":[{"rank":1,"name":"Bot_3","score":4,"answered":6},
            {"rank":2,"name":"Ananya","score":2,"answered":5}],
 "players":2,"you":"Ananya",
 "cache":{"hits":17,"misses":1,"hit_rate":0.94,"genres":2,
          "questions_pooled":20,"llm_calls":2}}
```

The same message is **pushed unsolicited** to every remaining client whenever
somebody leaves, with an extra `"note"` field naming who disconnected.

### 5.6 Liveness

`KEEP_ALIVE` and `ALIVE_OK` are header-only, 12 bytes each, `length = 0`. The
server sends a probe after 5 s of silence on the socket and drops the client if
the next 5-second window also passes without any message. Any message counts as
proof of life, not just `ALIVE_OK`, so a client that is busy answering is never
mistaken for a dead one.

### 5.7 Ending

Client → server, type `7`, empty payload.
Server → client, type `7`:

```json
{"entries":[…],"players":3,"final_score":4,"answered":9,
 "reason":"client_quit","you":"Ananya","cache":{…}}
```

---

## 6. Framing rules the implementation follows

1. Header and payload are concatenated and written with a **single** `send()`
   loop, so a frame is never interleaved with another thread's frame on the
   same socket.
2. Receivers loop until all 12 header bytes and all `length` payload bytes have
   arrived. A short `recv()` is normal on TCP and is not an error.
3. `SO_RCVTIMEO` is 5 s on the server. A timeout **before** the first byte of a
   frame is the keep-alive trigger; a timeout **inside** a frame is retried so a
   message is never truncated.
4. Bad magic, wrong version or `length > 1 MiB` closes the connection.
5. `SIGPIPE` is ignored and `MSG_NOSIGNAL` is used, so writing to a client that
   has already gone away cannot kill the server.
