// =====================================================================
//  CS3530 Assignment-1 - quiz client (same source for Part A and Part B)
//  Roll number : cs23btech11047
//
//  Two modes:
//    interactive (default)  keyboard driven CLI quiz
//    --auto                 self-driving bot that picks a random genre and
//                           random answers, used for the concurrency sweep
//
//  A single select() loop watches the socket and stdin, so the client
//  never blocks on getline() while the server is talking to it.
// =====================================================================
#include "protocol.h"

static const char* DEFAULT_HOST = "192.168.50.111";
static const int   DEFAULT_PORT = 11047;

static const char* GENRES[] = {"Science", "History", "Maths", "Geography",
                               "Movies", "Music", "Sports", "Literature",
                               "Art", "Technology"};
static const int   NUM_GENRES = 10;

struct Options {
    string host      = DEFAULT_HOST;
    int    port      = DEFAULT_PORT;
    string user;
    string genre;
    bool   automatic = false;
    int    duration  = 120;
    string metrics;
    int    clients_tag = 1;
    bool   quiet     = false;
};

struct Metrics {
    FILE*  fp = nullptr;
    string client;
    int    n_clients = 1;

    void open(const string& path, const string& who, int n) {
        if (path.empty()) return;
        client = who; n_clients = n;
        fp = fopen(path.c_str(), "w");
        if (!fp) { perror("metrics file"); return; }
        fprintf(fp, "unix_time,client,n_clients,event,latency_ms,cache,"
                    "bytes_sent,bytes_recv,duration_s,throughput_Bps\n");
    }
    void event(const char* name, double latency_ms, const char* cache) {
        if (!fp) return;
        fprintf(fp, "%.6f,%s,%d,%s,%.3f,%s,,,,\n",
                nowSeconds(), client.c_str(), n_clients, name, latency_ms, cache);
        fflush(fp);
    }
    void session(double duration_s) {
        if (!fp) return;
        long long s = counters().sent.load(), r = counters().received.load();
        double bps = duration_s > 0 ? (double)(s + r) / duration_s : 0.0;
        fprintf(fp, "%.6f,%s,%d,session,,,%lld,%lld,%.3f,%.2f\n",
                nowSeconds(), client.c_str(), n_clients, s, r, duration_s, bps);
        fflush(fp);
        fclose(fp);
        fp = nullptr;
    }
};

static Options  opt;
static Metrics  metrics;
static int      sock = -1;
static bool     running = true;
static int      score = 0;
static int      answered = 0;
static bool     have_question = false;

// timestamps used for the client side latency measurements
static double t_question_request = 0.0;
static double t_answer_sent      = 0.0;
static double t_leaderboard_req  = 0.0;

static void printUsage() {
    cout <<
    "Usage: ./client [options]\n"
    "  --host <ip>        server address (default " << DEFAULT_HOST << ")\n"
    "  --port <n>         server port (default " << DEFAULT_PORT << ")\n"
    "  --user <name>      username (asked interactively when omitted)\n"
    "  --genre <name>     starting genre\n"
    "  --auto             run as an automated bot (random genre and answers)\n"
    "  --duration <sec>   bot run time, default 120\n"
    "  --metrics <file>   write a per-client CSV of latency and throughput\n"
    "  --clients <n>      value recorded in the n_clients CSV column\n"
    "  --quiet            bot mode: only print the final summary\n"
    "  --help\n";
}

static bool parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        auto next = [&](const char* what) -> string {
            if (i + 1 >= argc) { cerr << "missing value after " << what << endl; exit(2); }
            return string(argv[++i]);
        };
        if      (a == "--host")    opt.host = next("--host");
        else if (a == "--port")    opt.port = atoi(next("--port").c_str());
        else if (a == "--user")    opt.user = next("--user");
        else if (a == "--genre")   opt.genre = next("--genre");
        else if (a == "--auto")    opt.automatic = true;
        else if (a == "--duration")opt.duration = atoi(next("--duration").c_str());
        else if (a == "--metrics") opt.metrics = next("--metrics");
        else if (a == "--clients") opt.clients_tag = atoi(next("--clients").c_str());
        else if (a == "--quiet")   opt.quiet = true;
        else if (a == "--help")  { printUsage(); return false; }
        else { cerr << "unknown option " << a << endl; printUsage(); exit(2); }
    }
    return true;
}

static void say(const string& s) { if (!opt.quiet) cout << s << endl; }

static void requestQuestion(const string& genre) {
    t_question_request = nowSeconds();
    sendMessage(sock, GENRE_REQUEST, genre);
}

// ---------------------------------------------------------------------
static void renderQuestion(const string& payload, uint8_t flags) {
    json data = json::parse(payload);
    if (data.contains("error")) {
        cout << "\n  " << data["error"].get<string>() << "\n";
        have_question = false;
        return;
    }
    const bool cached = (flags & FLAG_CACHE_HIT) != 0;
    string origin = (flags & (FLAG_CACHE_HIT | FLAG_CACHE_MISS))
                  ? (cached ? "  [served from cache]" : "  [freshly generated]")
                  : "";

    string q = data.contains("q") ? data["q"].get<string>()
                                  : data.value("question", string("(no text)"));
    cout << "\n Question " << data.value("question_id", 0) << origin << "\n";
    cout << " " << q << "\n";
    if (data.contains("options"))
        for (const auto& o : data["options"]) cout << "   " << o.get<string>() << "\n";
    cout << " Answer with A, B, C or D.\n";
    have_question = true;
}

static void renderLeaderboard(const string& payload) {
    json lb = json::parse(payload);
    cout << "\n Leaderboard\n";
    if (lb.contains("note")) cout << " " << lb["note"].get<string>() << "\n";
    if (lb.contains("entries")) {
        for (const auto& e : lb["entries"]) {
            cout << "  " << e.value("rank", 0) << ". "
                 << e.value("name", string()) << "   "
                 << e.value("score", 0) << " / " << e.value("answered", 0) << "\n";
        }
        if (lb["entries"].empty()) cout << "  (nobody connected)\n";
    }
}

static void renderSummary(const string& payload) {
    json d = json::parse(payload);
    cout << "\n Quiz over - final scores available\n";
    if (d.contains("error")) { cout << " " << d["error"].get<string>() << "\n"; return; }
    cout << " Your score: " << d.value("final_score", 0)
         << " correct out of " << d.value("answered", 0) << "\n";
    cout << " Session ended because: " << d.value("reason", string("unknown")) << "\n";
    if (d.contains("entries")) {
        cout << " Final leaderboard\n";
        for (const auto& e : d["entries"])
            cout << "  " << e.value("rank", 0) << ". " << e.value("name", string())
                 << "   " << e.value("score", 0) << "\n";
    }
}

// ---------------------------------------------------------------------
static void handleServerMessage(const MessageHeader& h, const string& payload) {
    switch (h.type) {
        case SERVER_INFO: {
            json d = json::parse(payload);
            say(" Connected as '" + d.value("username", opt.user) + "'  |  session "
                + to_string(d.value("session_seconds", 300)) + "s  |  cache "
                + (d.value("cache_enabled", false) ? "on" : "off"));
            break;
        }
        case QUESTION_DELIVERY: {
            double ms = (nowSeconds() - t_question_request) * 1000.0;
            bool cached = (h.flags & FLAG_CACHE_HIT) != 0;
            metrics.event("question", ms, cached ? "hit" : "miss");
            if (opt.quiet) {
                // keep the bot output short but still traceable
                cout << "[" << opt.user << "] question in " << (long)ms << " ms "
                     << (cached ? "(cache)" : "(fresh)") << endl;
                json d = json::parse(payload);
                have_question = !d.contains("error");
            } else {
                renderQuestion(payload, h.flags);
                cout << " (question latency " << (long)ms << " ms)" << endl;
            }
            break;
        }
        case RESULT_DELIVERY: {
            double ms = (nowSeconds() - t_answer_sent) * 1000.0;
            metrics.event("answer", ms, "");
            json d = json::parse(payload);
            score    = d.value("score", score);
            answered = d.value("answered", answered);
            have_question = false;
            if (!opt.quiet) {
                cout << "\n " << d.value("message", string("?"));
                if (d.value("verdict", string()) == "wrong")
                    cout << "  (correct answer: " << d.value("correct_option", string("?")) << ")";
                cout << "   score " << score
                     << "   (result latency " << (long)ms << " ms)" << endl;
            }
            break;
        }
        case LEADERBOARD_DELIVERY: {
            if (t_leaderboard_req > 0) {
                metrics.event("leaderboard", (nowSeconds() - t_leaderboard_req) * 1000.0, "");
                t_leaderboard_req = 0;
            }
            if (!opt.quiet) renderLeaderboard(payload);
            break;
        }
        case ERROR_MSG: {
            json d = json::parse(payload);
            cout << " Server: " << d.value("error", payload) << endl;
            break;
        }
        case KEEP_ALIVE:
            sendMessage(sock, ALIVE_OK, "");
            if (!opt.quiet) cout << " (keep-alive answered)" << endl;
            break;
        case SESSION_END:
            renderSummary(payload);
            running = false;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------
static void interactiveCommand(string line) {
    string cmd = upperCopy(trimCopy(line));
    if (cmd.empty()) return;

    if (cmd == "Q") {
        cout << " Ending the session." << endl;
        sendMessage(sock, SESSION_END, "");
        return;
    }
    if (cmd == "L") {
        t_leaderboard_req = nowSeconds();
        sendMessage(sock, LEADERBOARD_REQUEST, "");
        return;
    }
    if (cmd == "N") { requestQuestion(opt.genre); return; }
    if (cmd.rfind("G ", 0) == 0) {
        opt.genre = trimCopy(line.substr(2));
        cout << " Genre changed to " << opt.genre << endl;
        requestQuestion(opt.genre);
        return;
    }
    if (cmd.size() == 1 && cmd[0] >= 'A' && cmd[0] <= 'D') {
        if (!have_question) { cout << " No question on screen. Press N for one." << endl; return; }
        t_answer_sent = nowSeconds();
        sendMessage(sock, ANSWER_SUBMISSION, cmd);
        return;
    }
    cout << " Commands: A B C D to answer, N next question, "
            "G <genre> change genre, L leaderboard, Q quit." << endl;
}

// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    if (!parseArgs(argc, argv)) return 0;
    srand((unsigned)(getpid() ^ (unsigned)(nowSeconds() * 1000)));

    if (opt.user.empty()) {
        if (opt.automatic) { opt.user = "Bot_" + to_string(getpid()); }
        else { cout << "Username: "; if (!getline(cin, opt.user)) return 0; }
    }
    opt.user = trimCopy(opt.user);
    if (opt.user.empty()) opt.user = "player";

    if (opt.genre.empty()) {
        if (opt.automatic) opt.genre = GENRES[rand() % NUM_GENRES];
        else { cout << "Quiz genre (e.g. Science, History, Sports): ";
               getline(cin, opt.genre); opt.genre = trimCopy(opt.genre); }
    }
    if (opt.genre.empty()) opt.genre = "General Knowledge";

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    sockaddr_in hint;
    memset(&hint, 0, sizeof(hint));
    hint.sin_family = AF_INET;
    hint.sin_port   = htons(opt.port);
    if (inet_pton(AF_INET, opt.host.c_str(), &hint.sin_addr) != 1) {
        cerr << "Bad server address: " << opt.host << endl; return -1;
    }
    if (connect(sock, (sockaddr*)&hint, sizeof(hint)) != 0) {
        cerr << "Cannot reach the quiz server at " << opt.host << ":" << opt.port
             << ". Start the server first." << endl;
        close(sock);
        return -4;
    }
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;   // keeps recv responsive
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    metrics.open(opt.metrics, opt.user, opt.clients_tag);
    double started = nowSeconds();

    sendMessage(sock, USERNAME_SUBMISSION, opt.user);
    say(" Connected to " + opt.host + ":" + to_string(opt.port));
    requestQuestion(opt.genre);

    if (!opt.automatic)
        cout << "\n Commands: A B C D answer | N next question | G <genre> change genre"
                " | L leaderboard | Q quit\n" << endl;

    // bot scheduling state
    double next_bot_action = nowSeconds() + 1.0;
    int    bot_questions   = 0;
    bool   quit_sent       = false;

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        int maxfd = sock;
        if (!opt.automatic) { FD_SET(STDIN_FILENO, &rfds); maxfd = max(maxfd, STDIN_FILENO); }

        struct timeval sel; sel.tv_sec = 0; sel.tv_usec = 200000;
        int ready = select(maxfd + 1, &rfds, nullptr, nullptr, &sel);
        if (ready < 0) { if (errno == EINTR) continue; perror("select"); break; }

        if (FD_ISSET(sock, &rfds)) {
            MessageHeader h; string payload;
            int st = recvMessage(sock, h, payload, true);
            if (st == RECV_CLOSED) { cout << "\n Server closed the connection." << endl; break; }
            if (st == RECV_OK) {
                try { handleServerMessage(h, payload); }
                catch (const std::exception& e) {
                    cerr << " Could not read a server message: " << e.what() << endl;
                }
            } else if (st == RECV_ERROR || st == RECV_BADMSG) {
                cerr << " Connection problem (status " << st << ")." << endl;
                break;
            }
        }

        if (!opt.automatic && FD_ISSET(STDIN_FILENO, &rfds)) {
            string line;
            if (!getline(cin, line)) { sendMessage(sock, SESSION_END, ""); continue; }
            interactiveCommand(line);
        }

        if (opt.automatic) {
            double now = nowSeconds();
            if (!quit_sent && now - started >= opt.duration) {
                sendMessage(sock, SESSION_END, "");
                quit_sent = true;
                next_bot_action = now + 5.0;      // grace period for the summary
            } else if (quit_sent && now > next_bot_action) {
                break;
            } else if (!quit_sent && now >= next_bot_action) {
                if (have_question) {
                    string pick(1, (char)('A' + rand() % 4));
                    t_answer_sent = nowSeconds();
                    sendMessage(sock, ANSWER_SUBMISSION, pick);
                    have_question = false;
                    bot_questions++;
                    next_bot_action = now + 0.5 + (rand() % 1500) / 1000.0;
                } else {
                    if (bot_questions > 0 && bot_questions % 5 == 0) {
                        t_leaderboard_req = nowSeconds();
                        sendMessage(sock, LEADERBOARD_REQUEST, "");
                        bot_questions++;          // avoid asking twice in a row
                    }
                    requestQuestion(opt.genre);
                    next_bot_action = now + 1.0 + (rand() % 2000) / 1000.0;
                }
            }
        }
    }

    double elapsed = nowSeconds() - started;
    metrics.session(elapsed);

    long long s = counters().sent.load(), r = counters().received.load();
    cout << "\n[" << opt.user << "] session " << (long)elapsed << " s | sent "
         << s << " B | received " << r << " B | application throughput "
         << (long)((s + r) / (elapsed > 0 ? elapsed : 1)) << " B/s"
         << " | score " << score << endl;

    shutdown(sock, SHUT_RDWR);
    close(sock);
    return 0;
}
