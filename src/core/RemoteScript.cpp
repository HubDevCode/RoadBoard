#include "core/RemoteScript.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <regex>

#include "core/Params.h"

namespace road {

namespace {

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// split on ';' / newline outside quotes & parentheses, removing // comments
std::vector<std::string> splitStatements(const std::string& script) {
    std::vector<std::string> out;
    std::string cur;
    bool quote = false;
    int paren = 0;
    for (size_t i = 0; i < script.size(); ++i) {
        const char c = script[i];
        if (!quote && c == '/' && i + 1 < script.size() && script[i + 1] == '/') {
            while (i < script.size() && script[i] != '\n') ++i;
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
            continue;
        }
        if (c == '"') quote = !quote;
        if (!quote && c == '(') ++paren;
        if (!quote && c == ')') --paren;
        if (!quote && paren <= 0 && (c == ';' || c == '\n')) {
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
    return v;
}

bool parseNumber(const std::string& s, float* out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return false;
    while (*end == ' ') ++end;
    if (*end != '\0') return false;
    *out = float(d);
    return true;
}

std::string fmt(float v, bool integer) {
    char buf[32];
    if (integer) std::snprintf(buf, sizeof buf, "%d", int(std::lround(v)));
    else std::snprintf(buf, sizeof buf, "%.3f", double(v));
    return buf;
}

} // namespace

RemoteScript& RemoteScript::instance() {
    static RemoteScript r;
    return r;
}

void RemoteScript::registerAction(const std::string& key, Action a) {
    std::lock_guard<std::mutex> lk(mutex_);
    actions_[ParamStore::normalise(key)] = std::move(a);
}

std::string RemoteScript::resolveAlias(const std::string& key) const {
    ParamStore& P = params();
    static const std::regex eqRe(R"(^bus\[(\d+)\]\.eq\.channel\[(\d+)\]\.(.*)$)");
    std::smatch m;
    if (std::regex_match(key, m, eqRe)) {
        const int b = std::stoi(m[1]);
        if (b >= 0 && b < kBuses) {
            const int ab = P.bus[b].eqAB->i();
            return "bus[" + m[1].str() + "].eq.ab[" + std::to_string(ab) + "].channel[" + m[2].str() + "]." + m[3].str();
        }
    }
    static const std::regex stripEqRe(R"(^strip\[(\d+)\]\.eq\.channel\[(\d+)\]\.(.*)$)");
    if (std::regex_match(key, m, stripEqRe)) {
        const int s = std::stoi(m[1]);
        if (s >= 0 && s < kHwStrips) {
            const int ab = P.strip[s].eqAB->i();
            return "strip[" + m[1].str() + "].eq.ab[" + std::to_string(ab) + "].channel[" + m[2].str() + "]." + m[3].str();
        }
    }
    static const std::regex devRe(R"(^(strip|bus)\[(\d+)\]\.device(\..*)?$)");
    if (std::regex_match(key, m, devRe)) return m[1].str() + "[" + m[2].str() + "].device";
    if (key.rfind("option.buffer.", 0) == 0) return "option.buffer";
    if (key == "strip[0].audibility") return key;
    return key;
}

bool RemoteScript::executeOne(const std::string& stmtIn, std::string* errors) {
    std::string stmt = trim(stmtIn);
    if (stmt.empty()) return false;
    ParamStore& P = params();

    // operator
    size_t opPos = std::string::npos;
    int op = 0; // 0 '=', 1 '+=', 2 '-='
    bool quote = false;
    for (size_t i = 0; i < stmt.size(); ++i) {
        if (stmt[i] == '"') quote = !quote;
        if (quote || stmt[i] != '=') continue;
        opPos = i;
        if (i > 0 && stmt[i - 1] == '+') { op = 1; --opPos; }
        else if (i > 0 && stmt[i - 1] == '-') { op = 2; --opPos; }
        break;
    }
    std::string name, value;
    if (opPos == std::string::npos) {
        // forms like "Wait(1000)" or bare action "Command.Restart"
        name = stmt; value = "1";
    } else {
        name = stmt.substr(0, opPos);
        value = trim(stmt.substr(opPos + (op ? 2 : 1)));
    }
    std::string key = ParamStore::normalise(name);

    // ----- fades
    for (const char* kind : {"fadeto", "fadeby"}) {
        const std::string suffix = std::string(".") + kind;
        if (key.size() > suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
            const std::string target = key.substr(0, key.size() - suffix.size()) + ".gain";
            Param* p = P.find(target);
            std::string v = trim(value);
            if (!v.empty() && v.front() == '(') v = v.substr(1);
            if (!v.empty() && v.back() == ')') v.pop_back();
            const size_t comma = v.find(',');
            float db = 0, ms = 0;
            if (!p || comma == std::string::npos || !parseNumber(v.substr(0, comma), &db) || !parseNumber(v.substr(comma + 1), &ms)) {
                if (errors) *errors += "bad fade request: " + stmt + "\n";
                return false;
            }
            const float to = std::string(kind) == "fadeby" ? p->get() + db : db;
            std::lock_guard<std::mutex> lk(mutex_);
            fades_.erase(std::remove_if(fades_.begin(), fades_.end(), [&](const Fade& f) { return f.key == p->key; }), fades_.end());
            if (ms <= 0) p->set(to);
            else fades_.push_back({p->key, p->get(), to, nowSec(), std::min(ms, 120000.0f) / 1000.0});
            return true;
        }
    }

    // ----- bus modes: Bus(0).mode.Amix = 1
    static const std::regex modeRe(R"(^bus\[(\d+)\]\.mode\.([a-z0-9]+)$)");
    std::smatch m;
    if (std::regex_match(key, m, modeRe)) {
        const int b = std::stoi(m[1]);
        std::string mode = m[2];
        if (mode == "righttonly") mode = "rightonly";
        for (int i = 0; i < int(BusMode::Count); ++i) {
            if (mode == kBusModeKeys[i] && b >= 0 && b < kBuses) {
                float v = 1;
                parseNumber(value, &v);
                if (v >= 0.5f) P.bus[b].mode->set(float(i));
                else if (P.bus[b].mode->i() == i) P.bus[b].mode->set(0);
                return true;
            }
        }
    }

    key = resolveAlias(key);

    // ----- actions registered on an exact key win over parameters
    //       (e.g. "recorder.play" is both a status parameter and an action)
    Action exact;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = actions_.find(key);
        if (it != actions_.end()) exact = it->second;
    }
    if (exact) {
        float v = 1;
        const std::string arg = unquote(value);
        parseNumber(arg, &v);
        exact(arg, v, -1);
        return true;
    }

    // ----- numeric parameter
    if (Param* p = P.find(key)) {
        float v = 0;
        if (!parseNumber(unquote(value), &v)) {
            if (errors) *errors += "bad value: " + stmt + "\n";
            return false;
        }
        if (op == 0) p->set(v);
        else {
            const float delta = op == 1 ? v : -v;
            if (p->integer && p->min == 0 && p->max == 1) p->set(p->on() ? 0 : 1); // toggle
            else p->set(p->get() + delta);
        }
        return true;
    }

    // ----- string parameter
    if (P.hasStr(key)) {
        P.setStr(key, unquote(value));
        return true;
    }

    // ----- actions
    Action action;
    int index = -1;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = actions_.find(key);
        if (it != actions_.end()) action = it->second;
        else {
            static const std::regex idxRe(R"(\[(\d+)\])");
            if (std::regex_search(key, m, idxRe)) {
                index = std::stoi(m[1]);
                const std::string generic = m.prefix().str() + "[]" + m.suffix().str();
                auto it2 = actions_.find(generic);
                if (it2 != actions_.end()) action = it2->second;
                else if (std::regex_search(generic, m, idxRe)) {   // two indices: i * 1000 + j
                    index = index * 1000 + std::stoi(m[1]);
                    auto it3 = actions_.find(m.prefix().str() + "[]" + m.suffix().str());
                    if (it3 != actions_.end()) action = it3->second;
                }
            }
        }
    }
    if (action) {
        float v = 1;
        const std::string arg = unquote(value);
        parseNumber(arg, &v);
        action(arg, v, index);
        return true;
    }
    if (errors) *errors += "unknown request: " + stmt + "\n";
    return false;
}

int RemoteScript::execute(const std::string& script, std::string* errors) {
    int ok = 0;
    for (const std::string& s : splitStatements(script)) ok += executeOne(s, errors) ? 1 : 0;
    return ok;
}

bool RemoteScript::get(const std::string& name, std::string* out) {
    ParamStore& P = params();
    std::string key = ParamStore::normalise(name);
    std::smatch m;
    static const std::regex modeRe(R"(^bus\[(\d+)\]\.mode\.([a-z0-9]+)$)");
    if (std::regex_match(key, m, modeRe)) {
        const int b = std::stoi(m[1]);
        for (int i = 0; i < int(BusMode::Count); ++i)
            if (m[2].str() == kBusModeKeys[i] && b >= 0 && b < kBuses) { *out = P.bus[b].mode->i() == i ? "1" : "0"; return true; }
    }
    key = resolveAlias(key);
    if (Param* p = P.find(key)) { *out = fmt(p->get(), p->integer); return true; }
    if (P.hasStr(key)) { *out = P.str(key); return true; }
    return false;
}

void RemoteScript::tick() {
    std::vector<std::pair<std::string, float>> apply;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (fades_.empty()) return;
        const double t = nowSec();
        for (auto it = fades_.begin(); it != fades_.end();) {
            const double k = std::min(1.0, (t - it->start) / it->duration);
            apply.push_back({it->key, float(it->from + (it->to - it->from) * k)});
            if (k >= 1.0) it = fades_.erase(it); else ++it;
        }
    }
    for (auto& [k, v] : apply) if (Param* p = params().find(k)) p->set(v);
}

} // namespace road
