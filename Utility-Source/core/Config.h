#pragma once
#include <map>
#include <string>
#include <vector>

namespace glassboot {

// \glassboot\glassboot.cfg on the data partition (format: docs/CONFIG.md; read at boot by
// gb_config in boot/grub-module/glassboot.c, whose rules this follows).
//
// Editing keeps the user's file intact: serialize() rewrites changed lines in place, drops
// removed settings, appends new ones, and keeps comments and unknown lines.
class Config {
public:
    enum class Kind { Alias, Hide, Args, Persist, Class, Kickstart, Preseed, Autoinstall, AutoYast };

    // Global keys: "timeout", "default", "view", "theme", "password", "language".
    static const std::vector<std::string>& globalKeys();

    static Config parse(const std::string& text);
    std::string serialize() const;

    // Empty string = not set.
    std::string global(const std::string& key) const;
    void setGlobal(const std::string& key, const std::string& value);

    // Per-file settings. PATH is on the data partition, "/" or "\" separated, matched
    // case-insensitively. For Kind::Hide the value is ignored ("1" = hidden).
    std::string value(Kind kind, const std::string& path) const;
    void setValue(Kind kind, const std::string& path, const std::string& value);
    bool hidden(const std::string& path) const;            // this path exactly
    bool hiddenByParent(const std::string& path) const;    // a folder above it is hidden
    void setHidden(const std::string& path, bool hide);
    std::vector<std::string> paths(Kind kind) const;

    // "/Dir\sub/file.iso/" -> "/Dir/sub/file.iso"
    static std::string normPath(const std::string& p);

private:
    struct Line {
        std::string text;     // original text (without newline)
        int kind = -1;        // -1 other, -2 global, else Kind
        std::string key;      // global key (lowercase) or normalized path
    };
    struct Item {
        Kind kind;
        std::string path;     // normalized, original case
        std::string value;
    };

    const Item* find(Kind kind, const std::string& path) const;
    static std::string formatItem(const Item& it);
    static std::string formatGlobal(const std::string& key, const std::string& value);

    std::vector<Line> lines_;
    std::map<std::string, std::string> globals_;
    std::vector<Item> items_;
    bool crlf_ = false;
};

} // namespace glassboot
