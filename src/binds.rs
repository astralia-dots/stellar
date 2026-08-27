use std::process::Command;

#[derive(Clone)]
pub struct Keybind {
    pub modmask: u16,
    pub key: String,
    pub dispatcher: String,
    pub arg: String,
    pub submap: String,
}

impl Keybind {
    fn from_json(v: &serde_json::Value) -> Self {
        Self {
            modmask: v["modmask"].as_u64().unwrap_or(0) as u16,
            key: v["key"].as_str().unwrap_or_default().to_string(),
            dispatcher: v["dispatcher"].as_str().unwrap_or_default().to_string(),
            arg: v["arg"].as_str().unwrap_or_default().to_string(),
            submap: v["submap"].as_str().unwrap_or_default().to_string(),
        }
    }
}

/// Shells out to `hyprctl binds -j` rather than using the `hyprland` crate:
/// that crate hardcodes the pre-XDG socket path (`/tmp/hypr/...`), which no
/// longer matches current Hyprland (`$XDG_RUNTIME_DIR/hypr/...`) -- verified
/// by hitting that exact mismatch when running this against Hyprland 0.56.2.
/// `hyprctl` ships with Hyprland itself, so it can't drift out of sync with
/// the compositor's own IPC the way a third-party wrapper crate can.
pub fn fetch() -> Result<Vec<Keybind>, String> {
    let output = Command::new("hyprctl")
        .args(["binds", "-j"])
        .output()
        .map_err(|e| format!("failed to run `hyprctl`: {e}"))?;
    if !output.status.success() {
        return Err(format!(
            "`hyprctl binds -j` exited with {}: {}",
            output.status,
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    let parsed: Vec<serde_json::Value> = serde_json::from_slice(&output.stdout)
        .map_err(|e| format!("failed to parse `hyprctl binds -j` output: {e}"))?;
    Ok(parsed.iter().map(Keybind::from_json).collect())
}
