#include "watch.h"

#include <cstdlib>

#include <sys/inotify.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t MASK = IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                          IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;

fs::path config_dir() {
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char* home = std::getenv("HOME"); home && *home)
        base = fs::path(home) / ".config";
    else
        return {};
    return base / "hypr";
}

} // namespace

ConfigWatcher::ConfigWatcher() : fd_(inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
    if (fd_ < 0)
        return;
    if (fs::path dir = config_dir(); !dir.empty())
        add_tree(dir);
}

ConfigWatcher::~ConfigWatcher() {
    if (fd_ >= 0)
        close(fd_);
}

void ConfigWatcher::add_tree(const fs::path& dir) {
    std::error_code ec;
    fs::path real = fs::canonical(dir, ec);
    if (ec || !seen_.insert(real).second)
        return;
    int wd = inotify_add_watch(fd_, real.c_str(), MASK);
    if (wd < 0)
        return;
    dirs_[wd] = real;

    for (fs::directory_iterator it(real, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        std::error_code e; // own error code: a bad entry must not end the loop
        if (fs::is_directory(p, e)) {
            add_tree(p);
        } else if (it->is_symlink(e)) {
            // symlinked file: watch its target
            fs::path target = fs::canonical(p, e);
            if (!e)
                inotify_add_watch(fd_, target.c_str(), MASK);
        }
    }
}

bool ConfigWatcher::drain() {
    bool changed = false;
    alignas(inotify_event) char buf[4096];
    for (;;) {
        ssize_t n = read(fd_, buf, sizeof buf);
        if (n <= 0)
            break; // EAGAIN: nothing left
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<inotify_event*>(p);
            p += sizeof(inotify_event) + ev->len;
            if (ev->mask & IN_IGNORED) {
                dirs_.erase(ev->wd);
                continue;
            }
            changed = true;
            // watch directories created or moved in later
            if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) && (ev->mask & IN_ISDIR) && ev->len)
                if (auto d = dirs_.find(ev->wd); d != dirs_.end())
                    add_tree(d->second / ev->name);
        }
    }
    return changed;
}
