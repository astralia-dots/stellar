#include "binds.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

using json = nlohmann::json;

std::string str_field(const json& o, const char* k) {
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

struct Output {
    int status = 0;
    std::string out, err;
};

std::expected<Output, std::string> run(const char* const argv[]) {
    int out[2], err[2];
    if (pipe2(out, O_CLOEXEC) < 0)
        return std::unexpected(std::strerror(errno));
    if (pipe2(err, O_CLOEXEC) < 0) {
        close(out[0]);
        close(out[1]);
        return std::unexpected(std::strerror(errno));
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, err[1], STDERR_FILENO);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    close(err[1]);
    if (rc != 0) {
        close(out[0]);
        close(err[0]);
        return std::unexpected(std::strerror(rc));
    }

    Output res;
    pollfd fds[] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    std::string* sinks[] = {&res.out, &res.err};
    for (int open = 2; open > 0;) {
        if (poll(fds, 2, -1) < 0 && errno != EINTR)
            break;
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !fds[i].revents)
                continue;
            char buf[4096];
            ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                sinks[i]->append(buf, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                close(fds[i].fd);
                fds[i].fd = -1; // poll() skips negative fds
                --open;
            }
        }
    }
    for (auto& f : fds)
        if (f.fd >= 0)
            close(f.fd);
    waitpid(pid, &res.status, 0);
    return res;
}

/// Prefix like `Super + Ctrl + ` for a Hyprland modmask (X11 bits), empty for 0.
/// Order follows `B.mod` in the Lua config.
std::string modifiers(uint16_t mask) {
    static constexpr struct {
        uint16_t bit;
        const char* name;
    } NAMES[] = {{64, "Super"}, {4, "Ctrl"}, {8, "Alt"}, {1, "Shift"},
                 {2, "Caps"}, {16, "Mod2"}, {32, "Mod3"}, {128, "Mod5"}};
    std::string s;
    for (const auto& [bit, name] : NAMES)
        if (mask & bit)
            s += std::string(name) + " + ";
    return s;
}

/// Code point count, so padding also lines up for non-ASCII key names.
size_t utf8_len(const std::string& s) {
    return static_cast<size_t>(std::ranges::count_if(s, [](char c) { return (c & 0xc0) != 0x80; }));
}

std::string describe(int status) {
    if (WIFEXITED(status))
        return std::format("exit status: {}", WEXITSTATUS(status));
    if (WIFSIGNALED(status))
        return std::format("signal: {}", WTERMSIG(status));
    return std::format("status: {}", status);
}

} // namespace

BindResult parse_binds(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (!j.is_array())
        return std::unexpected("failed to parse `hyprctl binds -j` output: expected a JSON array");
    std::vector<Keybind> binds;
    for (const json& o : j) {
        if (!o.is_object()) {
            binds.emplace_back();
            continue;
        }
        Keybind b;
        auto m = o.find("modmask");
        if (m != o.end() && m->is_number_unsigned())
            b.modmask = static_cast<uint16_t>(m->get<uint64_t>());
        b.key = str_field(o, "key");
        b.dispatcher = str_field(o, "dispatcher");
        b.arg = str_field(o, "arg");
        b.submap = str_field(o, "submap");
        binds.push_back(std::move(b));
    }
    return binds;
}

BindResult fetch() {
    const char* const argv[] = {"hyprctl", "binds", "-j", nullptr};
    auto res = run(argv);
    if (!res)
        return std::unexpected(std::format("failed to run `hyprctl`: {}", res.error()));
    if (!WIFEXITED(res->status) || WEXITSTATUS(res->status) != 0)
        return std::unexpected(std::format("`hyprctl binds -j` exited with {}: {}",
                                           describe(res->status), res->err));
    return parse_binds(res->out);
}

std::vector<std::string> format_binds(const std::vector<Keybind>& binds) {
    std::vector<std::string> keys, lines;
    size_t width = 0;
    for (const Keybind& b : binds) {
        std::string k = modifiers(b.modmask) + b.key;
        if (!b.submap.empty())
            k = std::format("[{}] {}", b.submap, k);
        width = std::max(width, utf8_len(k));
        keys.push_back(std::move(k));
    }
    for (size_t i = 0; i < binds.size(); ++i) {
        std::string action = binds[i].dispatcher;
        if (!binds[i].arg.empty())
            action += " " + binds[i].arg;
        lines.push_back(std::format("{}{} : {}", keys[i], std::string(width - utf8_len(keys[i]), ' '), action));
    }
    return lines;
}
