// =====================================================================
//  CS3530 Assignment-1 - Part 1(A): quiz server WITHOUT cache
//  Roll number : cs23btech11047      Listening port : 11047
//
//  One detached POSIX thread per client, at most 10 concurrent clients,
//  5 minute sessions, KEEP_ALIVE liveness probing every 5 seconds and a
//  shared leaderboard guarded by a mutex. Every GENRE_REQUEST triggers a
//  fresh call to the LLM backend - that is the whole point of Part A and
//  the baseline that Part B is compared against.
// =====================================================================
#include "protocol.h"
#include "llm.h"

static const int    SERVER_PORT        = 11047;
static const int    MAX_CLIENTS        = 10;
static const int    SESSION_SECONDS    = 300;   // 5 minutes
static const int    KEEPALIVE_SECONDS  = 5;     // probe interval == recv timeout
static const int    LLM_RETRIES        = 3;

map<string, ClientSession> sessions;              // username -> session
pthread_mutex_t            sessions_mutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<int>           active_clients(0);
std::atomic<long long>     llm_calls(0);

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOG(x) do {                                                        \
        pthread_mutex_lock(&log_mutex);                                    \
        cout << "[" << timestampString() << "] " << x << endl;             \
        pthread_mutex_unlock(&log_mutex);                                  \
    } while (0)

// ---------------------------------------------------------------------
// Leaderboard helpers. Both must be called with sessions_mutex held.
// ---------------------------------------------------------------------
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

static void broadcastLeaderboardLocked(const string& note, const string& skip) {
    json lb = leaderboardJsonLocked();
    lb["note"] = note;
    string payload = lb.dump();
    for (map<string, ClientSession>::const_iterator it = sessions.begin();
         it != sessions.end(); ++it) {
        if (it->first == skip) continue;
        sendMessage(it->second.socket, LEADERBOARD_DELIVERY, payload);
    }
}

static void sendSessionEnd(int fd, const string& client_id, const string& reason) {
    pthread_mutex_lock(&sessions_mutex);
    json summary = leaderboardJsonLocked();
    summary["final_score"] = sessions.count(client_id) ? sessions[client_id].score : 0;
    summary["answered"]    = sessions.count(client_id) ? sessions[client_id].answered : 0;
    pthread_mutex_unlock(&sessions_mutex);
    summary["reason"] = reason;
    summary["you"]    = client_id;
    sendJson(fd, SESSION_END, summary);
}

// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);          // never die because a client vanished
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
    LOG("Part A server listening on port " << port
        << " (no cache, max " << MAX_CLIENTS << " clients, "
        << SESSION_SECONDS << "s sessions)");

    while (true) {
        sockaddr_in cliAddr;
        socklen_t   cliLen = sizeof(cliAddr);
        int clientSocket = accept(serverSocket, (sockaddr*)&cliAddr, &cliLen);
        if (clientSocket < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &cliAddr.sin_addr, ip, sizeof(ip));
        string peer = string(ip) + ":" + to_string(ntohs(cliAddr.sin_port));

        int nodelay = 1;   // small messages: do not wait for Nagle
        setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        if (active_clients.load() >= MAX_CLIENTS) {
            LOG("client " << peer << " rejected - server full ("
                << MAX_CLIENTS << " active)");
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
            close(clientSocket);
            delete data;
            active_clients--;
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
    int    clientSocket = data->clientSocket;
    string peer         = peerString(clientSocket);
    string client_id;
    string correct_answer;
    int    question_no  = 0;
    bool   awaiting_pong = false;
    bool   registered    = false;
    string end_reason    = "client_disconnected";

    // ---- handshake: USERNAME_SUBMISSION (10 s budget) ----------------
    {
        struct timeval hs; hs.tv_sec = 10; hs.tv_usec = 0;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &hs, sizeof(hs));

        MessageHeader h; string payload;
        int st = recvMessage(clientSocket, h, payload, true);
        if (st != RECV_OK || h.type != USERNAME_SUBMISSION || trimCopy(payload).empty()) {
            LOG("handshake failed for " << peer << " (status " << st << ")");
            close(clientSocket);
            active_clients--;
            return nullptr;
        }
        client_id = trimCopy(payload).substr(0, 32);
    }

    // ---- register, making the display name unique --------------------
    pthread_mutex_lock(&sessions_mutex);
    {
        string base = client_id;
        int suffix = 2;
        while (sessions.count(client_id)) client_id = base + "#" + to_string(suffix++);
        ClientSession& s = sessions[client_id];
        s.socket    = clientSocket;
        s.score     = 0;
        s.answered  = 0;
        s.peer      = peer;
        s.joined_at = nowSeconds();
        registered  = true;
    }
    pthread_mutex_unlock(&sessions_mutex);

    LOG("client " << peer << " registered as '" << client_id << "'. Session started.");

    struct timeval tv; tv.tv_sec = KEEPALIVE_SECONDS; tv.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    json info;
    info["username"]          = client_id;
    info["session_seconds"]   = SESSION_SECONDS;
    info["keepalive_seconds"] = KEEPALIVE_SECONDS;
    info["cache_enabled"]     = false;
    info["mode"]              = "PART_A";
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
            // No traffic for KEEPALIVE_SECONDS. Probe once; if the next
            // window also passes with silence, declare the client dead.
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
        if (st == RECV_CLOSED) {
            LOG("client '" << client_id << "' closed the connection");
            break;
        }
        if (st != RECV_OK) {
            LOG("client '" << client_id << "' transport error (status " << st << ")");
            break;
        }

        awaiting_pong = false;   // any frame proves the client is alive

        switch (header.type) {
            case ALIVE_OK:
                LOG("ALIVE_OK <- '" << client_id << "'");
                break;

            case GENRE_REQUEST: {
                string genre = trimCopy(payload);
                if (genre.empty()) genre = "general knowledge";
                LOG("GENRE_REQUEST from '" << client_id << "' : " << genre
                    << "  (no cache - calling LLM)");

                double t0 = nowSeconds();
                json out;
                bool ok = false;

                for (int attempt = 1; attempt <= LLM_RETRIES && !ok; ++attempt) {
                    try {
                        llm_calls++;
                        string raw = getQuizRaw(genre, 1);
                        vector<QuizQuestion> qs = parseQuizResponse(raw);
                        QuizQuestion q = qs[0];
                        correct_answer = q.a;
                        question_no++;
                        out["question_id"] = question_no;
                        out["question"]    = q.q;
                        out["options"]     = q.options;
                        out["genre"]       = genre;
                        out["from_cache"]  = false;
                        ok = true;
                    } catch (const std::exception& e) {
                        LOG("LLM attempt " << attempt << "/" << LLM_RETRIES
                            << " for '" << client_id << "' failed: " << e.what());
                    }
                }

                double ms = (nowSeconds() - t0) * 1000.0;
                if (ok) {
                    LOG("question " << question_no << " delivered to '" << client_id
                        << "' in " << (long)ms << " ms (LLM round trip)");
                    sendJson(clientSocket, QUESTION_DELIVERY, out);
                } else {
                    json err;
                    err["error"] = "The quiz generator did not return a usable "
                                   "question. Try another genre.";
                    LOG("giving up on genre '" << genre << "' for '" << client_id << "'");
                    sendJson(clientSocket, QUESTION_DELIVERY, err);
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
                lb["you"] = client_id;
                LOG("LEADERBOARD_REQUEST from '" << client_id << "'");
                sendJson(clientSocket, LEADERBOARD_DELIVERY, lb);
                break;
            }

            case SESSION_END: {
                LOG("client '" << client_id << "' asked to end the session");
                end_reason = "client_quit";
                sendSessionEnd(clientSocket, client_id, end_reason);
                goto cleanup;
            }

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
        broadcastLeaderboardLocked("'" + client_id + "' left the quiz", "");
        size_t remaining = sessions.size();
        pthread_mutex_unlock(&sessions_mutex);
        LOG("removed '" << client_id << "' (final score " << final_score
            << ", reason " << end_reason << "); leaderboard pushed to "
            << remaining << " client(s)");
    }

    close(clientSocket);
    active_clients--;
    LOG("client " << peer << " disconnected. Active clients: " << active_clients.load()
        << " | total LLM calls: " << llm_calls.load());
    return nullptr;
}
