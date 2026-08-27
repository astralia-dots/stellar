mod binds;
mod ui;
mod watch;

use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn pid_file() -> PathBuf {
    let runtime_dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
    PathBuf::from(runtime_dir).join("stellar.pid")
}

fn running_pid() -> Option<u32> {
    let path = pid_file();
    let pid: u32 = fs::read_to_string(&path).ok()?.trim().parse().ok()?;
    if PathBuf::from(format!("/proc/{pid}")).exists() {
        Some(pid)
    } else {
        // Stale pid file from a crashed run.
        let _ = fs::remove_file(&path);
        None
    }
}

fn main() {
    if let Some(pid) = running_pid() {
        // Toggle off: signal the running instance to close, then exit.
        let _ = Command::new("kill").arg("-TERM").arg(pid.to_string()).status();
        return;
    }

    fs::write(pid_file(), std::process::id().to_string()).expect("failed to write pid file");
    // No cleanup-on-exit here: the default SIGTERM disposition (the only
    // way this process ever quits, via the toggle-off path above) kills
    // the process without running Rust destructors -- verified against a
    // real toggle cycle, not assumed. The pid file is left stale on
    // purpose; `running_pid()` above already detects and clears a stale
    // file (checked against `/proc/<pid>`) on the next launch.
    ui::run();
}
