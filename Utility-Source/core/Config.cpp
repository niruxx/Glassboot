#include "Config.h"

#include <algorithm>
#include <set>

namespace glassboot {
namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

const char* kindName(Config::Kind k) {
    switch (k) {
    case Config::Kind::Alias: return "alias";
    case Config::Kind::Hide: return "hide";
    case Config::Kind::Args: return "args";
    case Config::Kind::Persist: return "persist";
    case Config::Kind::Class: return "class";
    }
    return "";
}

bool kindFromName(const std::string& name, Config::Kind& k) {
    std::string n = lower(name);
    if (n == "alias") k = Config::Kind::Alias;
    else if (n == "hide") k = Config::Kind::Hide;
    else if (n == "args") k = Config::Kind::Args;
    else if (n == "persist") k = Config::Kind::Persist;
    else if (n == "class") k = Config::Kind::Class;
    else return false;
    return true;
}

// Split a setting line the way gb_config does: first word, then "=" (optional for the
// first word), then the rest.
void splitLine(const std::string& line, std::string& key, std::string& rest) {
    size_t i = 0;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '=') ++i;
    key = line.substr(0, i);
    std::string r = i < line.size() ? line.substr(i) : std::string();
    r = trim(r);
    if (!r.empty() && r[0] == '=') r = r.substr(1);
    rest = trim(r);
}

} // namespace

const std::vector<std::string>& Config::globalKeys() {
    static const std::vector<std::string> keys{"timeout", "default", "view", "theme", "password", "language"};
    return keys;
}

std::string Config::normPath(const std::string& p) {
    std::string r;
    std::string t = trim(p);
    if (t.empty() || (t[0] != '/' && t[0] != '\\')) r = "/";
    for (char c : t) r += c == '\\' ? '/' : c;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    return r;
}

Config Config::parse(const std::string& textIn) {
    Config c;
    std::string text = textIn;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text = text.substr(3);
    c.crlf_ = text.find("\r\n") != std::string::npos;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();

        Line line;
        line.text = raw;
        std::string t = trim(raw);
        if (!t.empty() && t[0] != '#') {
            std::string key, rest;
            splitLine(t, key, rest);
            Kind kind;
            std::string lk = lower(key);
            if (std::find(globalKeys().begin(), globalKeys().end(), lk) != globalKeys().end()) {
                line.kind = -2;
                line.key = lk;
                if (lk == "default" || lk == "theme") rest = normPath(rest);
                c.globals_[lk] = rest; // the last one wins, as at boot
            } else if (kindFromName(key, kind)) {
                Item it{kind, {}, {}};
                if (kind == Kind::Hide) {
                    it.path = normPath(rest);
                    it.value = "1";
                } else {
                    size_t eq = rest.find('=');
                    if (eq != std::string::npos) {
                        it.path = normPath(rest.substr(0, eq));
                        it.value = trim(rest.substr(eq + 1));
                        if (kind == Kind::Persist) it.value = normPath(it.value);
                    }
                }
                if (!it.path.empty()) {
                    line.kind = int(kind);
                    line.key = lower(it.path);
                    // At boot the first matching entry wins.
                    if (!c.find(kind, it.path)) c.items_.push_back(it);
                }
            }
        }
        c.lines_.push_back(line);
    }
    return c;
}

const Config::Item* Config::find(Kind kind, const std::string& path) const {
    std::string lp = lower(normPath(path));
    for (const auto& it : items_)
        if (it.kind == kind && lower(it.path) == lp) return &it;
    return nullptr;
}

std::string Config::global(const std::string& key) const {
    auto it = globals_.find(lower(key));
    return it == globals_.end() ? std::string() : it->second;
}

void Config::setGlobal(const std::string& key, const std::string& value) {
    std::string k = lower(key), v = trim(value);
    if (v.empty()) {
        globals_.erase(k);
        return;
    }
    if (k == "default" || k == "theme") v = normPath(v); // as gb_config reads them
    globals_[k] = v;
}

std::string Config::value(Kind kind, const std::string& path) const {
    const Item* it = find(kind, path);
    return it ? it->value : std::string();
}

void Config::setValue(Kind kind, const std::string& path, const std::string& valueIn) {
    std::string value = trim(valueIn);
    if (kind == Kind::Persist && !value.empty()) value = normPath(value);
    std::string lp = lower(normPath(path));
    auto it = std::find_if(items_.begin(), items_.end(),
                           [&](const Item& i) { return i.kind == kind && lower(i.path) == lp; });
    if (value.empty()) {
        if (it != items_.end()) items_.erase(it);
    } else if (it != items_.end()) {
        it->value = value;
    } else {
        items_.push_back({kind, normPath(path), value});
    }
}

bool Config::hidden(const std::string& path) const { return find(Kind::Hide, path) != nullptr; }

bool Config::hiddenByParent(const std::string& path) const {
    std::string lp = lower(normPath(path));
    for (const auto& it : items_) {
        if (it.kind != Kind::Hide) continue;
        std::string h = lower(it.path);
        if (lp.size() > h.size() && lp.compare(0, h.size(), h) == 0 && (h == "/" || lp[h.size()] == '/'))
            return true;
    }
    return false;
}

void Config::setHidden(const std::string& path, bool hide) { setValue(Kind::Hide, path, hide ? "1" : ""); }

std::vector<std::string> Config::paths(Kind kind) const {
    std::vector<std::string> out;
    for (const auto& it : items_)
        if (it.kind == kind) out.push_back(it.path);
    return out;
}

std::string Config::formatGlobal(const std::string& key, const std::string& value) {
    return key + " = " + value;
}

std::string Config::formatItem(const Item& it) {
    if (it.kind == Kind::Hide) return std::string("hide ") + it.path;
    return std::string(kindName(it.kind)) + " " + it.path + " = " + it.value;
}

std::string Config::serialize() const {
    std::vector<std::string> out;
    std::set<std::string> doneGlobals;
    std::set<std::pair<int, std::string>> doneItems;

    for (const auto& line : lines_) {
        if (line.kind == -1) {
            out.push_back(line.text);
        } else if (line.kind == -2) {
            if (doneGlobals.count(line.key)) continue; // duplicates collapse into one
            auto g = globals_.find(line.key);
            if (g == globals_.end()) continue;        // removed
            out.push_back(formatGlobal(line.key, g->second));
            doneGlobals.insert(line.key);
        } else {
            std::pair<int, std::string> id{line.kind, line.key};
            if (doneItems.count(id)) continue;
            const Item* it = find(Kind(line.kind), line.key);
            if (!it) continue;
            out.push_back(formatItem(*it));
            doneItems.insert(id);
        }
    }

    // New settings go at the end; a fresh file gets a short header first.
    std::vector<std::string> added;
    for (const auto& key : globalKeys()) {
        auto g = globals_.find(key);
        if (g != globals_.end() && !doneGlobals.count(key)) added.push_back(formatGlobal(key, g->second));
    }
    for (const auto& it : items_)
        if (!doneItems.count({int(it.kind), lower(it.path)})) added.push_back(formatItem(it));
    if (!added.empty()) {
        if (lines_.empty()) {
            out.push_back("# Glassboot configuration (see docs/CONFIG.md).");
            out.push_back("# Written by the Glassboot Installer; comments and unknown lines are kept.");
            out.push_back("");
        }
        while (!out.empty() && trim(out.back()).empty() && !lines_.empty()) out.pop_back();
        if (!lines_.empty() && !out.empty()) out.push_back("");
        out.insert(out.end(), added.begin(), added.end());
    }

    std::string nl = crlf_ ? "\r\n" : "\n";
    std::string s;
    for (const auto& l : out) s += l + nl;
    return s;
}

} // namespace glassboot
