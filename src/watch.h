#pragma once

#include <filesystem>
#include <set>
#include <unordered_map>

/// Watches `~/.config/hypr/` with `inotify`: poll `fd()`, then call `drain()`.
/// Symlinks under it also get their resolved target watched, because dotfile
/// setups keep the real files outside the watched tree.
///
/// ponytail: any change under the watched paths counts, not only the files
/// Hyprland reads; an unrelated edit costs one wasted reload.
class ConfigWatcher {
public:
    ConfigWatcher();
    ~ConfigWatcher();
    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    /// -1 if `inotify` failed to initialise (`poll()` skips it).
    int fd() const { return fd_; }

    /// Reads all pending events; true if anything changed.
    bool drain();

private:
    void add_tree(const std::filesystem::path& dir);

    int fd_;
    std::unordered_map<int, std::filesystem::path> dirs_; // watch descriptor -> directory
    std::set<std::filesystem::path> seen_;                // canonical dirs, guards against symlink loops
};
