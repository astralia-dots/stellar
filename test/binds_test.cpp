#include "binds.h"

#include <cstdio>
#include <string>

#define CHECK(x)                                                     \
    do {                                                             \
        if (!(x)) {                                                  \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); \
            return 1;                                                \
        }                                                            \
    } while (0)

int main() {
    auto ok = parse_binds(R"([
        {"modmask": 64, "key": "Q", "dispatcher": "exec", "arg": "kitty", "submap": ""},
        {"modmask": 65, "key": "H", "dispatcher": "movefocus", "arg": "l", "submap": "resize"},
        {"key": "F1"}
    ])");
    CHECK(ok && ok->size() == 3);
    // The widest keys column is "[resize] Super + Shift + H" (26 chars); the others pad to it.
    auto lines = format_binds(*ok);
    CHECK(lines.size() == 3);
    CHECK(lines[0] == "Super + Q" + std::string(17, ' ') + " : exec kitty");
    CHECK(lines[1] == "[resize] Super + Shift + H : movefocus l");
    CHECK(lines[2] == "F1" + std::string(24, ' ') + " : "); // missing fields default to empty/zero

    Keybind all;
    all.modmask = 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128;
    all.key = "X";
    CHECK(format_binds({all})[0] == "Super + Ctrl + Alt + Shift + Caps + Mod2 + Mod3 + Mod5 + X : ");

    CHECK(parse_binds("[]") && parse_binds("[]")->empty());
    CHECK(!parse_binds("not json"));
    CHECK(!parse_binds("{}"));
    return 0;
}
