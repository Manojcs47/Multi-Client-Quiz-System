// =====================================================================
//  CS3530 Assignment-1 - Part 1(B): quiz server WITH server-side cache
//  Roll number : cs23btech11047      Listening port : 11047
//
//  Everything in Part A, plus a process-wide question cache keyed by
//  genre. One LLM call fetches a batch of 10 questions; every later
//  request for that genre is answered from memory until the entry is
//  invalidated.
//
//  Cache invalidation policy (three rules, justified in REPORT.md):
//    1. TTL          - an entry older than CACHE_TTL_MINUTES is dropped,
//                      which bounds how stale a question pool can get.
//    2. Exhaustion   - when a client has already seen every question in
//                      the pool, the entry is refilled from the LLM so
//                      nobody is served the same question twice.
//    3. LRU capacity - at most MAX_CACHED_GENRES entries; the least
//                      recently used genre is evicted, bounding memory.
//
//  A per-genre "fetch in progress" set plus a condition variable stops
//  N concurrent clients from firing N identical requests at the LLM on a
//  cold cache (thundering herd); the others wait and then get a hit.
// =====================================================================
#include "protocol.h"
#include "llm.h"

static const int SERVER_PORT        = 11047;
static const int MAX_CLIENTS        = 10;
static const int SESSION_SECONDS    = 300;
static const int KEEPALIVE_SECONDS  = 5;
static const int LLM_RETRIES        = 3;
static const int QUESTIONS_PER_FETCH= 10;
static const int CACHE_TTL_MINUTES  = 15;
static const int MAX_CACHED_GENRES  = 32;

map<string, ClientSession> sessions;
pthread_mutex_t            sessions_mutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<int>           active_clients(0);

struct CacheEntry {
    vector<QuizQuestion> questions;
    double created      = 0.0;   // for the TTL rule
    double last_access  = 0.0;   // for the LRU rule
    long long served    = 0;
    int       refills   = 0;
};

map<string, CacheEntry> quiz_cache;
set<string>             fetch_in_progress;
pthread_mutex_t         cache_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t          cache_cv    = PTHREAD_COND_INITIALIZER;

std::atomic<long long> cache_hits(0), cache_misses(0), llm_calls(0);

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOG(x) do {                                                        \
        pthread_mutex_lock(&log_mutex);                                    \
        cout << "[" << timestampString() << "] " << x << endl;             \
        pthread_mutex_unlock(&log_mutex);                                  \
    } while (0)

// ---------------------------------------------------------------------
//  leaderboard
// ---------------------------------------------------------------------
static json cacheStatsJson() {
    long long h = cache_hits.load(), m = cache_misses.load();
    pthread_mutex_lock(&cache_mutex);
    int genres = (int)quiz_cache.size();
    int pooled = 0;
    for (map<string, CacheEntry>::const_iterator it = quiz_cache.begin();
         it != quiz_cache.end(); ++it) pooled += (int)it->second.questions.size();
    pthread_mutex_unlock(&cache_mutex);
    return json{{"hits", h}, {"misses", m},
                {"hit_rate", (h + m) ? (double)h / (double)(h + m) : 0.0},
                {"genres", genres}, {"questions_pooled", pooled},
                {"llm_calls", llm_calls.load()}};
}

static json leaderboardJsonLocked() {
    vector<pair<string, ClientSession> > rows(sessions.begin(), sessions.end());
    sort(rows.begin(), rows.end(),
         [](const pair<string, ClientSession>& a, const pair<string, ClientSession>& b) {
             if (a.second.score != b.second.score) return a.second.score > b.second.score;
             return a.first < b.first;
         });
    json entries = json::array();
    for (size_t i = 0; i < rows.size(); ++i) {
        entries.push_back(json{{"rank",     (int)i + 1},
                               {"name",     rows[i].first},
                               {"score",    rows[i].second.score},
                               {"answered", rows[i].second.answered}});
    }
    return json{{"entries", entries}, {"players", (int)rows.size()}};
}

static void broadcastLeaderboardLocked(const string& note) {
    json lb = leaderboardJsonLocked();
    lb["note"] = note;
    string payload = lb.dump();
    for (map<string, ClientSession>::const_iterator it = sessions.begin();
         it != sessions.end(); ++it)
        sendMessage(it->second.socket, LEADERBOARD_DELIVERY, payload);
}

static void sendSessionEnd(int fd, const string& client_id, const string& reason) {
    pthread_mutex_lock(&sessions_mutex);
    json summary = leaderboardJsonLocked();
    summary["final_score"] = sessions.count(client_id) ? sessions[client_id].score : 0;
    summary["answered"]    = sessions.count(client_id) ? sessions[client_id].answered : 0;
    pthread_mutex_unlock(&sessions_mutex);
    summary["reason"] = reason;
    summary["you"]    = client_id;
    summary["cache"]  = cacheStatsJson();
    sendJson(fd, SESSION_END, summary);
}

// ---------------------------------------------------------------------
//  cache maintenance (all helpers assume cache_mutex is held)
// ---------------------------------------------------------------------
static void dropExpiredLocked(const string& genre) {
    map<string, CacheEntry>::iterator it = quiz_cache.find(genre);
    if (it == quiz_cache.end()) return;
    double age_min = (nowSeconds() - it->second.created) / 60.0;
    if (age_min > CACHE_TTL_MINUTES) {
        LOG("CACHE INVALIDATE genre='" << genre << "' reason=ttl age="
            << (long)age_min << "min");
        quiz_cache.erase(it);
    }
}

static void enforceCapacityLocked() {
    while (quiz_cache.size() > (size_t)MAX_CACHED_GENRES) {
        map<string, CacheEntry>::iterator victim = quiz_cache.end();
        for (map<string, CacheEntry>::iterator it = quiz_cache.begin();
             it != quiz_cache.end(); ++it) {
            if (fetch_in_progress.count(it->first)) continue;
            if (victim == quiz_cache.end() ||
                it->second.last_access < victim->second.last_access) victim = it;
        }
        if (victim == quiz_cache.end()) break;
        LOG("CACHE EVICT genre='" << victim->first << "' reason=lru capacity="
            << MAX_CACHED_GENRES);
        quiz_cache.erase(victim);
    }
}

// Fetch a batch from the LLM. Called with cache_mutex NOT held.
static bool fetchBatch(const string& genre, vector<QuizQuestion>& out) {
    for (int attempt = 1; attempt <= LLM_RETRIES; ++attempt) {
        try {
            llm_calls++;
            double t0 = nowSeconds();
            string raw = getQuizRaw(genre, QUESTIONS_PER_FETCH);
            out = parseQuizResponse(raw);
            LOG("LLM returned " << out.size() << " question(s) for genre='" << genre
                << "' in " << (long)((nowSeconds() - t0) * 1000) << " ms");
            return true;
        } catch (const std::exception& e) {
            LOG("LLM attempt " << attempt << "/" << LLM_RETRIES
                << " for genre='" << genre << "' failed: " << e.what());
        }
    }
    return false;
}

// Pick a question the caller has not seen yet, refilling the pool when
// necessary. was_fresh tells the caller whether an LLM call happened,
// which becomes the CACHE_HIT / CACHE_MISS header flag.
static bool getQuestion(const string& genre, const string& client_id,
                        set<string>& seen, QuizQuestion& out, bool& was_fresh) {
    was_fresh = false;
    int refills = 0;
    int loops   = 0;

    pthread_mutex_lock(&cache_mutex);
    while (true) {
        if (++loops > 8) {              // safety net: never spin on a bad genre
            pthread_mutex_unlock(&cache_mutex);
            cache_misses++;
            LOG("CACHE GIVE UP genre='" << genre << "' client='" << client_id << "'");
            return false;
        }
        dropExpiredLocked(genre);

        map<string, CacheEntry>::iterator it = quiz_cache.find(genre);
        if (it != quiz_cache.end()) {
            for (size_t i = 0; i < it->second.questions.size(); ++i) {
                const QuizQuestion& q = it->second.questions[i];
                if (seen.count(lowerCopy(q.q))) continue;
                out = q;
                it->second.last_access = nowSeconds();
                it->second.served++;
                long pool = (long)it->second.questions.size();
                long age  = (long)(nowSeconds() - it->second.created);
                pthread_mutex_unlock(&cache_mutex);
                if (was_fresh) {
                    cache_misses++;
                    LOG("CACHE MISS  genre='" << genre << "' client='" << client_id
                        << "' -> LLM called, pool now " << pool << " question(s)");
                } else {
                    cache_hits++;
                    LOG("CACHE HIT   genre='" << genre << "' client='" << client_id
                        << "' pool=" << pool << " age=" << age << "s (no LLM call)");
                }
                return true;
            }
            // pool exists but this client has seen all of it -> rule 2
            if (refills >= 2 && !it->second.questions.empty()) {
                LOG("CACHE REUSE genre='" << genre << "' client='" << client_id
                    << "' - pool exhausted twice, repeating questions");
                seen.clear();
                continue;
            }
        }

        if (fetch_in_progress.count(genre)) {
            // another thread is already asking the LLM for this genre
            LOG("CACHE WAIT  genre='" << genre << "' client='" << client_id
                << "' - a fetch is already in flight");
            pthread_cond_wait(&cache_cv, &cache_mutex);
            continue;
        }

        fetch_in_progress.insert(genre);
        pthread_mutex_unlock(&cache_mutex);

        vector<QuizQuestion> batch;
        bool ok = fetchBatch(genre, batch);

        pthread_mutex_lock(&cache_mutex);
        fetch_in_progress.erase(genre);
        pthread_cond_broadcast(&cache_cv);

        if (!ok) {
            pthread_mutex_unlock(&cache_mutex);
            cache_misses++;
            return false;
        }

        CacheEntry& entry = quiz_cache[genre];
        if (entry.created == 0.0) entry.created = nowSeconds();
        // merge: keep old questions, append the ones we do not have yet
        set<string> known;
        for (size_t i = 0; i < entry.questions.size(); ++i)
            known.insert(lowerCopy(entry.questions[i].q));
        int added = 0;
        for (size_t i = 0; i < batch.size(); ++i) {
            if (known.count(lowerCopy(batch[i].q))) continue;
            entry.questions.push_back(batch[i]);
            known.insert(lowerCopy(batch[i].q));
            added++;
        }
        entry.created     = nowSeconds();   // the pool was just refreshed
        entry.last_access = nowSeconds();
        entry.refills++;
        refills++;
        was_fresh = true;
        LOG("CACHE FILL  genre='" << genre << "' +" << added
            << " new question(s), pool=" << entry.questions.size());
        enforceCapacityLocked();
    }
}

// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    int port = SERVER_PORT;
    for (int i = 1; i < argc; ++i)
        if (string(argv[i]) == "--port" && i + 1 < argc) port = atoi(argv[++i]);

    cout << "Reserving the LLM instance for " << ROLL_NUMBER << " ..." << endl;
    if (!reserveLLM()) {
        cerr << "Could not reserve the LLM. The server will not start." << endl;
        curl_global_cleanup();
        return -1;
    }
    cout << "LLM reserved." << endl;

    pthread_t keeper;
    pthread_create(&keeper, nullptr, reservationKeeper, nullptr);
    pthread_detach(keeper);

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in serverAdr;
    memset(&serverAdr, 0, sizeof(serverAdr));
    serverAdr.sin_family      = AF_INET;
    serverAdr.sin_port        = htons(port);
    serverAdr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (sockaddr*)&serverAdr, sizeof(serverAdr)) < 0) {
        perror("bind"); close(serverSocket); return -2;
    }
    if (listen(serverSocket, 16) < 0) {
        perror("listen"); close(serverSocket); return -3;
    }
    LOG("Part B server listening on port " << port << " (cache on, TTL "
        << CACHE_TTL_MINUTES << " min, batch " << QUESTIONS_PER_FETCH
        << ", max " << MAX_CLIENTS << " clients)");

    while (true) {
        sockaddr_in cliAddr;
        socklen_t   cliLen = sizeof(cliAddr);
        int clientSocket = accept(serverSocket, (sockaddr*)&cliAddr, &cliLen);
        if (clientSocket < 0) { if (errno == EINTR) continue; perror("accept"); continue; }

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &cliAddr.sin_addr, ip, sizeof(ip));
        string peer = string(ip) + ":" + to_string(ntohs(cliAddr.sin_port));

        int nodelay = 1;
        setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        if (active_clients.load() >= MAX_CLIENTS) {
            LOG("client " << peer << " rejected - server full (" << MAX_CLIENTS << ")");
            json busy;
            busy["error"]  = "Server is full (10 clients). Please try again later.";
            busy["reason"] = "server_full";
            sendJson(clientSocket, SESSION_END, busy);
            close(clientSocket);
            continue;
        }

        active_clients++;
        LOG("client " << peer << " connected. Active clients: " << active_clients.load());

        ClientThreadData* data = new ClientThreadData();
        data->clientSocket = clientSocket;
        pthread_t thread;
        if (pthread_create(&thread, nullptr, handle_client, data) != 0) {
            perror("pthread_create");
            close(clientSocket); delete data; active_clients--;
            continue;
        }
        pthread_detach(thread);
    }

    close(serverSocket);
    curl_global_cleanup();
    return 0;
}

// ---------------------------------------------------------------------
void* handle_client(void* arg) {
    std::unique_ptr<ClientThreadData> data((ClientThreadData*)arg);
    int    clientSocket  = data->clientSocket;
    string peer          = peerString(clientSocket);
    string client_id;
    string correct_answer;
    string current_genre;
    set<string> seen_questions;          // per client, per genre
    int    question_no   = 0;
    bool   awaiting_pong = false;
    bool   registered    = false;
    string end_reason    = "client_disconnected";

    {
        struct timeval hs; hs.tv_sec = 10; hs.tv_usec = 0;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &hs, sizeof(hs));
        MessageHeader h; string payload;
        int st = recvMessage(clientSocket, h, payload, true);
        if (st != RECV_OK || h.type != USERNAME_SUBMISSION || trimCopy(payload).empty()) {
            LOG("handshake failed for " << peer << " (status " << st << ")");
            close(clientSocket); active_clients--; return nullptr;
        }
        client_id = trimCopy(payload).substr(0, 32);
    }

    pthread_mutex_lock(&sessions_mutex);
    {
        string base = client_id;
        int suffix = 2;
        while (sessions.count(client_id)) client_id = base + "#" + to_string(suffix++);
        ClientSession& s = sessions[client_id];
        s.socket = clientSocket; s.score = 0; s.answered = 0;
        s.peer = peer; s.joined_at = nowSeconds();
        registered = true;
    }
    pthread_mutex_unlock(&sessions_mutex);

    LOG("client " << peer << " registered as '" << client_id << "'. Session started.");

    struct timeval tv; tv.tv_sec = KEEPALIVE_SECONDS; tv.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    json info;
    info["username"]          = client_id;
    info["session_seconds"]   = SESSION_SECONDS;
    info["keepalive_seconds"] = KEEPALIVE_SECONDS;
    info["cache_enabled"]     = true;
    info["cache_ttl_minutes"] = CACHE_TTL_MINUTES;
    info["mode"]              = "PART_B";
    sendJson(clientSocket, SERVER_INFO, info);

    double session_start = nowSeconds();

    while (true) {
        if (nowSeconds() - session_start >= SESSION_SECONDS) {
            LOG("client '" << client_id << "' session finished (5 minute limit)");
            end_reason = "time_limit";
            sendSessionEnd(clientSocket, client_id, end_reason);
            break;
        }

        MessageHeader header; string payload;
        int st = recvMessage(clientSocket, header, payload, true);

        if (st == RECV_TIMEOUT) {
            if (awaiting_pong) {
                LOG("client '" << client_id << "' did not answer KEEP_ALIVE within "
                    << KEEPALIVE_SECONDS << "s - dropping");
                end_reason = "keepalive_timeout";
                break;
            }
            sendMessage(clientSocket, KEEP_ALIVE, "");
            awaiting_pong = true;
            LOG("KEEP_ALIVE -> '" << client_id << "'");
            continue;
        }
        if (st == RECV_CLOSED) { LOG("client '" << client_id << "' closed the connection"); break; }
        if (st != RECV_OK)     { LOG("client '" << client_id << "' transport error " << st); break; }

        awaiting_pong = false;

        switch (header.type) {
            case ALIVE_OK:
                LOG("ALIVE_OK <- '" << client_id << "'");
                break;

            case GENRE_REQUEST: {
                string genre = lowerCopy(trimCopy(payload));
                if (genre.empty()) genre = "general knowledge";
                if (genre != current_genre) {
                    current_genre = genre;
                    seen_questions.clear();
                }
                LOG("GENRE_REQUEST from '" << client_id << "' : " << genre);

                double t0 = nowSeconds();
                QuizQuestion q;
                bool was_fresh = false;
                bool ok = getQuestion(current_genre, client_id, seen_questions, q, was_fresh);
                double ms = (nowSeconds() - t0) * 1000.0;

                if (ok) {
                    seen_questions.insert(lowerCopy(q.q));
                    correct_answer = q.a;
                    question_no++;
                    json out;
                    out["question_id"] = question_no;
                    out["q"]           = q.q;
                    out["question"]    = q.q;          // convenience alias
                    out["options"]     = q.options;
                    out["genre"]       = payload;
                    out["from_cache"]  = !was_fresh;
                    uint8_t flag = was_fresh ? FLAG_CACHE_MISS : FLAG_CACHE_HIT;
                    LOG("question " << question_no << " -> '" << client_id << "' in "
                        << (long)ms << " ms, served "
                        << (was_fresh ? "after an LLM call" : "from cache"));
                    sendJson(clientSocket, QUESTION_DELIVERY, out, flag);
                } else {
                    json err;
                    err["error"] = "The quiz generator did not return a usable "
                                   "question. Try another genre.";
                    err["from_cache"] = false;
                    LOG("giving up on genre '" << genre << "' for '" << client_id << "'");
                    sendJson(clientSocket, QUESTION_DELIVERY, err, FLAG_CACHE_MISS);
                }
                break;
            }

            case ANSWER_SUBMISSION: {
                char given = answerLetter(payload);
                char want  = answerLetter(correct_answer);
                json result;
                if (correct_answer.empty()) {
                    result["verdict"] = "no_question";
                    result["message"] = "Ask for a question first.";
                } else if (given && given == want) {
                    pthread_mutex_lock(&sessions_mutex);
                    sessions[client_id].score++;
                    sessions[client_id].answered++;
                    result["score"] = sessions[client_id].score;
                    pthread_mutex_unlock(&sessions_mutex);
                    result["verdict"] = "correct";
                    result["message"] = "Correct";
                    result["correct_option"] = string(1, want);
                    correct_answer.clear();
                } else {
                    pthread_mutex_lock(&sessions_mutex);
                    sessions[client_id].answered++;
                    result["score"] = sessions[client_id].score;
                    pthread_mutex_unlock(&sessions_mutex);
                    result["verdict"] = "wrong";
                    result["message"] = "Wrong";
                    result["correct_option"] = string(1, want ? want : '?');
                    correct_answer.clear();
                }
                LOG("ANSWER '" << trimCopy(payload) << "' from '" << client_id
                    << "' -> " << result["verdict"].get<string>());
                sendJson(clientSocket, RESULT_DELIVERY, result);
                break;
            }

            case LEADERBOARD_REQUEST: {
                pthread_mutex_lock(&sessions_mutex);
                json lb = leaderboardJsonLocked();
                pthread_mutex_unlock(&sessions_mutex);
                lb["you"]   = client_id;
                lb["cache"] = cacheStatsJson();
                LOG("LEADERBOARD_REQUEST from '" << client_id << "'");
                sendJson(clientSocket, LEADERBOARD_DELIVERY, lb);
                break;
            }

            case SESSION_END:
                LOG("client '" << client_id << "' asked to end the session");
                end_reason = "client_quit";
                sendSessionEnd(clientSocket, client_id, end_reason);
                goto cleanup;

            default:
                LOG("unexpected message " << msgName(header.type)
                    << " from '" << client_id << "'");
                break;
        }
    }

cleanup:
    if (registered) {
        pthread_mutex_lock(&sessions_mutex);
        int final_score = sessions.count(client_id) ? sessions[client_id].score : 0;
        sessions.erase(client_id);
        broadcastLeaderboardLocked("'" + client_id + "' left the quiz");
        size_t remaining = sessions.size();
        pthread_mutex_unlock(&sessions_mutex);
        LOG("removed '" << client_id << "' (final score " << final_score
            << ", reason " << end_reason << "); leaderboard pushed to "
            << remaining << " client(s)");
    }

    close(clientSocket);
    active_clients--;
    json cs = cacheStatsJson();
    LOG("client " << peer << " disconnected. Active clients: " << active_clients.load()
        << " | cache hits " << cs["hits"] << " misses " << cs["misses"]
        << " hit-rate " << (int)(cs["hit_rate"].get<double>() * 100) << "%"
        << " | LLM calls " << llm_calls.load());
    return nullptr;
}
