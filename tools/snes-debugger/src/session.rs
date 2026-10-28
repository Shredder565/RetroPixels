//! The folder RetroPixels and the debugger share (internal/snesdbg in
//! RetroPixels): which game is playing, its ROM, capture requests and the
//! captures RetroPixels saves.

use serde::Deserialize;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// %LOCALAPPDATA%\RetroPixels SNES Debugger (RETRO_SNESDBG_DIR overrides it),
/// the same folder RetroPixels uses.
pub fn default_base() -> PathBuf {
    if let Ok(d) = std::env::var("RETRO_SNESDBG_DIR") {
        if !d.is_empty() {
            return PathBuf::from(d);
        }
    }
    let base = std::env::var("LOCALAPPDATA")
        .ok()
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var("XDG_CACHE_HOME").ok().filter(|s| !s.is_empty()).map(PathBuf::from))
        .or_else(|| std::env::var("HOME").ok().map(|h| Path::new(&h).join(".cache")))
        .unwrap_or_else(std::env::temp_dir);
    base.join("RetroPixels SNES Debugger")
}

#[derive(Deserialize, Default, Clone, Debug)]
pub struct Active {
    #[serde(default)]
    pub session: String,
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub app: String,
}

/// The game playing now, if any.
pub fn active(base: &Path) -> Option<Active> {
    let b = std::fs::read(base.join("active.json")).ok()?;
    let a: Active = serde_json::from_slice(&b).ok()?;
    (!a.session.is_empty()).then_some(a)
}

#[derive(Deserialize, Default, Clone, Debug)]
#[serde(rename_all = "camelCase")]
pub struct SessionInfo {
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub title: String,
    #[serde(default)]
    pub app: String,
    #[serde(default)]
    pub rom_sha1: String,
    #[serde(default)]
    pub started: i64,
}

pub fn session_info(dir: &Path) -> Option<SessionInfo> {
    serde_json::from_slice(&std::fs::read(dir.join("session.json")).ok()?).ok()
}

pub fn now_ms() -> u128 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0)
}

/// Tells RetroPixels the debugger is running (so it doesn't start another).
pub fn heartbeat(base: &Path) {
    let _ = std::fs::create_dir_all(base);
    let s = format!("{{\"pid\": {}, \"alive\": {}}}", std::process::id(), now_ms());
    let p = base.join("debugger.json");
    let tmp = base.join("debugger.json.tmp");
    if std::fs::write(&tmp, s).is_ok() {
        let _ = std::fs::rename(&tmp, &p);
    }
}

/// Asks the game for a capture; returns the capture's id (its folder name).
pub fn request_capture(session: &Path) -> Result<String, String> {
    let t = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
    let id = format!("c{}-{:03}", t.as_secs(), t.subsec_millis());
    let dir = session.join("requests");
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    std::fs::write(dir.join(format!("{id}.req")), "{\"kind\":\"capture\"}").map_err(|e| e.to_string())?;
    Ok(id)
}

/// Waits for capture id to appear.
pub fn wait_capture(session: &Path, id: &str, timeout: Duration) -> Option<PathBuf> {
    let dir = session.join("captures").join(id);
    let end = std::time::Instant::now() + timeout;
    while std::time::Instant::now() < end {
        if dir.join("capture.json").exists() {
            return Some(dir);
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    None
}

/// The session's captures, oldest first.
pub fn captures(session: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(session.join("captures"))
        .map(|rd| rd.flatten().map(|e| e.path()).filter(|p| p.is_dir() && p.join("capture.json").exists() && !p.file_name().unwrap().to_string_lossy().starts_with('.')).collect())
        .unwrap_or_default();
    v.sort();
    v
}

/// Earlier sessions, newest first.
pub fn sessions(base: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(base.join("sessions")).map(|rd| rd.flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect()).unwrap_or_default();
    v.sort();
    v.reverse();
    v
}
