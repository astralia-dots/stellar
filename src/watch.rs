use notify::{RecommendedWatcher, RecursiveMode, Watcher};
use std::path::{Path, PathBuf};

fn config_dir() -> PathBuf {
    let config_dir = std::env::var("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| {
            PathBuf::from(std::env::var("HOME").expect("HOME not set")).join(".config")
        });
    config_dir.join("hypr")
}

/// Dotfiles-managed configs commonly symlink individual files (or whole
/// subdirs) in from a separate repo -- confirmed on this machine, where
/// `hyprland.lua` and files under `devices/`/`utils/` are all symlinks into
/// `~/keqing-dots/...`. `notify`'s directory watch only sees changes to the
/// symlink entry itself, not writes at the resolved target outside the
/// watched tree, so each symlink's real target needs its own explicit watch.
fn watch_symlink_targets(watcher: &mut RecommendedWatcher, dir: &Path) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_symlink() {
            if let Ok(target) = std::fs::canonicalize(&path) {
                let mode = if target.is_dir() {
                    RecursiveMode::Recursive
                } else {
                    RecursiveMode::NonRecursive
                };
                let _ = watcher.watch(&target, mode);
            }
        } else if path.is_dir() {
            watch_symlink_targets(watcher, &path);
        }
    }
}

/// Watches `~/.config/hypr/` (and the real target of every symlink under
/// it, see `watch_symlink_targets`) and calls `on_reload` on every write
/// found. `hyprland-rs` 0.3 has no typed `ConfigReloaded` IPC event to
/// subscribe to instead, so this reacts to the same file save that
/// triggers Hyprland's own auto-reload. Covers `hyprland.conf` and
/// `hyprland.lua` (Hyprland supports both; this machine actually uses the
/// Lua config) plus any `source =`d files.
///
/// ponytail: fires on any change under the watched paths, not just the
/// file(s) actually in use, so an edit to an unrelated stray file also
/// triggers a (harmless, just wasted) re-render.
pub fn watch_config_reload<F: Fn() + Send + 'static>(on_reload: F) -> notify::Result<()> {
    let mut watcher = notify::recommended_watcher(move |res: notify::Result<notify::Event>| {
        if res.is_ok() {
            on_reload();
        }
    })?;
    let dir = config_dir();
    watcher.watch(&dir, RecursiveMode::Recursive)?;
    watch_symlink_targets(&mut watcher, &dir);
    // Leaked for the process lifetime: there's exactly one watcher per run
    // and it must outlive this function to keep delivering events.
    Box::leak(Box::new(watcher));
    Ok(())
}
