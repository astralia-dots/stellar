#include "ui.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;

static fs::path pid_file() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    return fs::path(dir ? dir : "/tmp") / "stellar.pid";
}

static std::optional<pid_t> running_pid() {
    pid_t pid = 0;
    if (!(std::ifstream(pid_file()) >> pid) || pid <= 0)
        return std::nullopt;
    if (fs::exists("/proc/" + std::to_string(pid)))
        return pid;
    // stale pid file from a crashed run
    std::error_code ec;
    fs::remove(pid_file(), ec);
    return std::nullopt;
}

int main() {
    if (auto pid = running_pid()) {
        // already running: toggle off
        kill(*pid, SIGTERM);
        return 0;
    }

    std::ofstream(pid_file()) << getpid();
    if (!fs::exists(pid_file())) {
        std::fprintf(stderr, "stellar: failed to write pid file\n");
        return 1;
    }
    // the pid file is never removed on exit: SIGTERM, the only way we quit,
    // skips cleanup, and `running_pid()` clears a stale file on the next launch
    return run_ui();
}
