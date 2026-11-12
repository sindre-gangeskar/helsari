//! The launcher's saved choices: where each game's dump is, where RPCS3 is, and which source tree
//! builds the games. Kept in %APPDATA%\Helsari\launcher.json.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::PathBuf;

#[derive(Clone, Default, Serialize, Deserialize)]
pub struct GameConfig {
    /// The disc dump folder (holds PS3_GAME).
    pub dump: Option<String>,
}

#[derive(Clone, Default, Serialize, Deserialize)]
pub struct Config {
    /// rpcs3.exe: decrypts the game executable once, with the player's installed firmware.
    pub rpcs3: Option<String>,
    /// The Helsari source tree the games are built from. For now the launcher builds in place
    /// (<tree>/build); later releases ship the runtime prebuilt and build elsewhere.
    pub source: Option<String>,
    #[serde(default)]
    pub games: BTreeMap<String, GameConfig>,
}

fn path() -> PathBuf {
    let base = std::env::var("APPDATA").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from("."));
    base.join("Helsari").join("launcher.json")
}

impl Config {
    pub fn load() -> Config {
        std::fs::read_to_string(path())
            .ok()
            .and_then(|s| serde_json::from_str(&s).ok())
            .unwrap_or_default()
    }

    pub fn save(&self) -> Result<(), String> {
        let p = path();
        if let Some(dir) = p.parent() {
            std::fs::create_dir_all(dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
        }
        let text = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        std::fs::write(&p, text).map_err(|e| format!("cannot write {}: {e}", p.display()))
    }

    /// The source tree: the saved one, or the tree this launcher was built from (development).
    pub fn source_dir(&self) -> PathBuf {
        match &self.source {
            Some(s) => PathBuf::from(s),
            None => PathBuf::from(env!("CARGO_MANIFEST_DIR")).parent().map(|p| p.to_path_buf()).unwrap_or_default(),
        }
    }

    pub fn build_dir(&self) -> PathBuf {
        self.source_dir().join("build")
    }
}
