// =====================================================================
//  CS3530 Assignment-1 : LLM backend integration (shared by Part A / B)
//  Roll number : cs23btech11047
//
//  Responsibilities
//    * reserve the shared LLM instance and renew the reservation before
//      the 5 minute idle timeout voids it,
//    * build the /generate_quiz request,
//    * parse the reply defensively (raw JSON, fenced JSON, truncated
//      JSON, or plain text) and validate every question.
// =====================================================================
#pragma once

#include "protocol.h"
#include <curl/curl.h>

// ------------------------- configuration -----------------------------
static const char* LLM_HOST     = "192.168.50.142";
static const int   LLM_PORT     = 25000;
static const char* ROLL_NUMBER  = "cs23btech11047";
static const long  LLM_TIMEOUT_SECONDS = 60;
// The reservation is voided after 5 minutes of idle time, so renew well
// inside that window. With caching (Part B) the server can otherwise sit
// idle long enough to silently lose the reservation.
static const int   RESERVATION_RENEW_SECONDS = 120;

// The three values above are the ones handed out for this course. They can
// be overridden without recompiling, which is handy when testing against a
// local stub instead of the shared lab instance:
//     QUIZ_LLM_HOST=127.0.0.1 QUIZ_LLM_PORT=25000 ./server
inline string envOr(const char* key, const string& fallback) {
    const char* v = getenv(key);
    return (v && *v) ? string(v) : fallback;
}

inline string llmUrl(const string& path) {
    return "http://" + envOr("QUIZ_LLM_HOST", LLM_HOST) + ":" +
           envOr("QUIZ_LLM_PORT", to_string(LLM_PORT)) + path;
}

inline string rollNumber() { return envOr("QUIZ_ROLLNO", ROLL_NUMBER); }

inline size_t llmWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Generic POST helper. Returns HTTP status code (0 on transport failure).
inline long llmPost(const string& url, const string& body, string& out) {
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    long http_code = 0;
    out.clear();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, llmWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, LLM_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);   // required: multi-threaded

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    } else {
        fprintf(stderr, "[LLM] curl failed for %s : %s\n",
                url.c_str(), curl_easy_strerror(res));
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return http_code;
}

inline bool reserveLLM() {
    json body;
    body["rollno"] = rollNumber();
    string reply;
    long code = llmPost(llmUrl("/reserve"), body.dump(), reply);
    if (code == 200) return true;
    cout << "[LLM] /reserve returned HTTP " << code
         << (reply.empty() ? "" : (" : " + reply)) << endl;
    return false;
}

// Background thread: renews the reservation so an idle server does not
// lose its slot. Started from main() after the first successful reserve.
inline void* reservationKeeper(void*) {
    while (true) {
        sleep(RESERVATION_RENEW_SECONDS);
        if (reserveLLM())
            cout << "[" << timestampString() << "] [LLM] reservation renewed" << endl;
        else
            cerr << "[" << timestampString() << "] [LLM] reservation renewal FAILED" << endl;
    }
    return nullptr;
}

// ------------------------- request construction ----------------------
inline string buildQuizRequest(const string& genre, int num_questions) {
    const string n = to_string(num_questions);

    string system_prompt =
        "You are a trivia generator. Given a genre, output exactly " + n +
        " multiple-choice questions. Each question must have 4 options "
        "(A, B, C, D) and one correct answer. Return ONLY valid JSON in the "
        "following format and nothing else: "
        "{\"questions\":[{\"q\":\"...\",\"options\":[\"A) ...\",\"B) ...\","
        "\"C) ...\",\"D) ...\"],\"a\":\"A\"}]}";

    json example;
    example["questions"] = json::array({
        json{{"q", "Who was the first President of the United States?"},
             {"options", json::array({"A) Thomas Jefferson", "B) Abraham Lincoln",
                                      "C) George Washington", "D) John Adams"})},
             {"a", "C"}}
    });

    json body;
    body["rollno"] = rollNumber();
    body["messages"] = json::array({
        json{{"role", "system"},    {"content", system_prompt}},
        json{{"role", "user"},      {"content", "Generate " + n +
                                                " MCQ trivia questions about History."}},
        json{{"role", "assistant"}, {"content", example.dump()}},
        json{{"role", "user"},      {"content", "Generate " + n +
                                                " MCQ trivia questions about " + genre + "."}}
    });
    body["temperature"] = 0.7;                       // variety across refills
    body["max_tokens"]  = num_questions <= 1 ? 400 : 2000;
    return body.dump();
}

// Returns the raw HTTP body of /generate_quiz ("" on failure).
// On HTTP 4xx the reservation has probably expired, so re-reserve once.
inline string getQuizRaw(const string& genre, int num_questions) {
    string reply;
    long code = llmPost(llmUrl("/generate_quiz"),
                        buildQuizRequest(genre, num_questions), reply);
    if (code == 200) return reply;

    cerr << "[LLM] /generate_quiz HTTP " << code << " - re-reserving" << endl;
    if (reserveLLM()) {
        code = llmPost(llmUrl("/generate_quiz"),
                       buildQuizRequest(genre, num_questions), reply);
        if (code == 200) return reply;
    }
    return "";
}

// ------------------------- response parsing --------------------------
// Pull the assistant text out of an OpenAI-style envelope. If the backend
// already answers with the bare quiz JSON, hand that back untouched.
inline string extractContent(const string& raw) {
    json parsed = json::parse(raw);                 // throws on garbage
    if (parsed.contains("choices") && parsed["choices"].is_array() &&
        !parsed["choices"].empty()) {
        const json& first = parsed["choices"][0];
        if (first.contains("message") && first["message"].contains("content"))
            return first["message"]["content"].get<string>();
        if (first.contains("text"))
            return first["text"].get<string>();
    }
    if (parsed.contains("questions") || parsed.contains("q")) return parsed.dump();
    if (parsed.contains("content"))  return parsed["content"].get<string>();
    if (parsed.contains("response")) return parsed["response"].get<string>();
    return parsed.dump();
}

// Drop ``` / ```json fences and any prose around the JSON object.
inline string stripFences(string s) {
    size_t f = s.find("```");
    while (f != string::npos) {
        size_t eol = s.find('\n', f);
        size_t end = (eol == string::npos) ? f + 3 : eol + 1;
        s.erase(f, end - f);
        f = s.find("```");
    }
    return s;
}

// Best effort repair of a truncated / slightly malformed JSON object:
// trims to the first '{', separates "}{"-glued objects, removes trailing
// commas and closes any brackets the model forgot.
inline string repairJson(string dirty) {
    size_t first = dirty.find('{');
    if (first == string::npos) return "{}";
    dirty.erase(0, first);

    size_t pos = dirty.find("}{");
    while (pos != string::npos) {
        dirty.replace(pos, 2, "},{");
        pos = dirty.find("}{", pos + 2);
    }

    string out;
    out.reserve(dirty.size() + 16);
    bool in_string = false;
    int  braces = 0, brackets = 0;
    size_t last_complete = 0;

    for (size_t i = 0; i < dirty.size(); ++i) {
        char c = dirty[i];
        if (c == '"' && (i == 0 || dirty[i - 1] != '\\')) in_string = !in_string;
        if (!in_string) {
            if (c == ',' && i + 1 < dirty.size()) {
                size_t k = dirty.find_first_not_of(" \t\r\n", i + 1);
                if (k != string::npos && (dirty[k] == '}' || dirty[k] == ']')) continue;
            }
            if (c == '{') braces++;
            if (c == '}') braces--;
            if (c == '[') brackets++;
            if (c == ']') brackets--;
        }
        out += c;
        if (!in_string && braces == 0 && brackets == 0 && c == '}') last_complete = out.size();
    }

    if (in_string) out += '"';
    if (braces == 0 && brackets == 0 && last_complete > 0) return out.substr(0, last_complete);
    while (brackets-- > 0) out += ']';
    while (braces--   > 0) out += '}';
    return out;
}

// Fallback for models that answer in prose:
//   1. Which planet is red?
//   A) Venus
//   ...
//   Answer: B
inline vector<QuizQuestion> parsePlainText(const string& text) {
    vector<QuizQuestion> out;
    stringstream ss(text);
    string line;
    QuizQuestion cur;

    while (getline(ss, line)) {
        line = trimCopy(line);
        if (line.empty()) continue;

        if (isdigit((unsigned char)line[0]) && line.find('.') != string::npos &&
            cur.options.empty()) {
            if (!cur.q.empty() && !cur.a.empty()) out.push_back(cur);
            cur = QuizQuestion();
            cur.q = trimCopy(line.substr(line.find('.') + 1));
        } else if (line.size() > 1 && line[0] >= 'A' && line[0] <= 'D' &&
                   (line[1] == ')' || line[1] == '.')) {
            cur.options.push_back(line);
        } else if (lowerCopy(line).rfind("answer", 0) == 0) {
            char c = answerLetter(line.substr(line.find_first_of(":") == string::npos
                                              ? 6 : line.find_first_of(":") + 1));
            if (c) {
                cur.a = string(1, c);
                out.push_back(cur);
                cur = QuizQuestion();
            }
        } else if (!cur.q.empty() && cur.options.empty()) {
            cur.q += " " + line;
        }
    }
    if (!cur.q.empty() && !cur.a.empty()) out.push_back(cur);
    return out;
}

// Keep only well-formed questions and normalise the answer to one letter.
inline void validateQuestions(vector<QuizQuestion>& qs) {
    vector<QuizQuestion> ok;
    set<string> seen;
    for (size_t i = 0; i < qs.size(); ++i) {
        QuizQuestion q = qs[i];
        q.q = trimCopy(q.q);
        if (q.q.size() < 5) continue;
        if (q.options.size() != 4) continue;

        bool blank = false;
        for (size_t k = 0; k < 4; ++k) {
            q.options[k] = trimCopy(q.options[k]);
            if (q.options[k].empty()) blank = true;
            // guarantee the "X) " prefix the client renders
            if (!q.options[k].empty() &&
                !(q.options[k][0] >= 'A' && q.options[k][0] <= 'D' &&
                  q.options[k].size() > 1 &&
                  (q.options[k][1] == ')' || q.options[k][1] == '.'))) {
                q.options[k] = string(1, (char)('A' + k)) + ") " + q.options[k];
            }
        }
        if (blank) continue;

        char letter = answerLetter(q.a);
        if (!letter) continue;
        q.a = string(1, letter);

        if (seen.count(lowerCopy(q.q))) continue;   // drop duplicates
        seen.insert(lowerCopy(q.q));
        ok.push_back(q);
    }
    qs.swap(ok);
}

// Full pipeline: HTTP body -> validated questions. Throws on total failure.
inline vector<QuizQuestion> parseQuizResponse(const string& raw_http_body) {
    if (trimCopy(raw_http_body).empty())
        throw runtime_error("empty response from LLM backend");

    string content = stripFences(extractContent(raw_http_body));
    vector<QuizQuestion> questions;

    if (content.find('{') != string::npos) {
        try {
            json data = json::parse(repairJson(content));
            if (data.contains("q") && data.contains("options"))     // single object
                data = json{{"questions", json::array({data})}};
            if (data.contains("questions") && data["questions"].is_array()) {
                for (const auto& item : data["questions"]) {
                    if (!item.is_object()) continue;
                    QuizQuestion q;
                    q.q = item.value("q", item.value("question", string()));
                    if (item.contains("options") && item["options"].is_array()) {
                        for (const auto& o : item["options"])
                            q.options.push_back(o.is_string() ? o.get<string>() : o.dump());
                    }
                    q.a = item.contains("a")
                              ? (item["a"].is_string() ? item["a"].get<string>() : item["a"].dump())
                              : item.value("answer", string());
                    questions.push_back(q);
                }
            }
        } catch (const std::exception&) {
            questions.clear();          // fall through to the text parser
        }
    }

    if (questions.empty()) questions = parsePlainText(content);

    validateQuestions(questions);
    if (questions.empty())
        throw runtime_error("no valid question could be parsed from the LLM reply");
    return questions;
}
