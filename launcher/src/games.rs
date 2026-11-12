//! The games the launcher knows, how it recognises a dump of each, and what the build needs.

use crate::config::Config;
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::process::Command;

pub struct Game {
    pub id: &'static str,
    pub title: &'static str,
    /// The disc's title ID: what a dump must be.
    pub title_id: &'static str,
    /// Whether this version of Helsari can build and run it.
    pub supported: bool,
    /// The executable the build produces, in the build folder.
    pub exe: &'static str,
}

pub const GAMES: &[Game] = &[
    Game { id: "helsari2", title: "Killzone 2", title_id: "BCES00081", supported: true, exe: "helsari2.exe" },
    Game { id: "helsari3", title: "Killzone 3", title_id: "", supported: false, exe: "helsari3.exe" },
];

pub fn game(id: &str) -> Option<&'static Game> {
    GAMES.iter().find(|g| g.id == id)
}

#[derive(Serialize, Clone)]
pub struct GameView {
    pub id: String,
    pub title: String,
    pub title_id: String,
    pub supported: bool,
    /// "unsupported", "missing" (no dump chosen or found), "ready" (dump found, not built), "built".
    pub status: String,
    pub dump: Option<String>,
    pub detail: String,
}

#[derive(Serialize, Clone)]
pub struct Prereq {
    pub name: String,
    pub ok: bool,
    pub detail: String,
    /// What the player can do about it: "pick-rpcs3", or a web page to open.
    pub action: Option<String>,
}

#[derive(Serialize, Clone)]
pub struct Overview {
    pub games: Vec<GameView>,
    pub prereqs: Vec<Prereq>,
    pub source: String,
}

/// One entry of a PARAM.SFO (the disc's metadata, a small key/value table), e.g. TITLE_ID.
fn sfo_value(sfo: &Path, key: &str) -> Option<String> {
    let d = std::fs::read(sfo).ok()?;
    if d.len() < 20 || &d[0..4] != b"\0PSF" {
        return None;
    }
    let u16_at = |o: usize| u16::from_le_bytes([d[o], d[o + 1]]) as usize;
    let u32_at = |o: usize| u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) as usize;
    let (keys, data, count) = (u32_at(8), u32_at(12), u32_at(16));
    for i in 0..count {
        let e = 20 + i * 16;
        if e + 16 > d.len() {
            break;
        }
        let key_at = keys + u16_at(e);
        let key_end = d[key_at..].iter().position(|&b| b == 0)? + key_at;
        if &d[key_at..key_end] == key.as_bytes() {
            let (len, at) = (u32_at(e + 4), data + u32_at(e + 12));
            let value = d.get(at..at + len)?;
            return Some(String::from_utf8_lossy(value).trim_end_matches('\0').to_string());
        }
    }
    None
}

/// The game's own menu background (PIC1.PNG), read from the player's dump when the card is shown and
/// never stored by the launcher. It goes to the window as a data URL.
#[derive(Serialize, Clone, Default)]
pub struct Art {
    pub background: Option<String>,
}

pub fn art(root: &Path) -> Art {
    let game_dir = root.join("PS3_GAME");
    let png = |name: &str| std::fs::read(game_dir.join(name)).ok().filter(|d| d.starts_with(&[0x89, b'P', b'N', b'G'])).map(|d| {
        format!("data:image/png;base64,{}", base64(&d))
    });
    Art {
        background: png("PIC1.PNG"),
    }
}

fn base64(data: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
    for chunk in data.chunks(3) {
        let b = [chunk[0], *chunk.get(1).unwrap_or(&0), *chunk.get(2).unwrap_or(&0)];
        let n = (b[0] as u32) << 16 | (b[1] as u32) << 8 | b[2] as u32;
        for (i, shift) in [18, 12, 6, 0].into_iter().enumerate() {
            out.push(if i <= chunk.len() { TABLE[(n >> shift & 63) as usize] as char } else { '=' });
        }
    }
    out
}

/// A dump folder as the player picked it: the folder holding PS3_GAME, or PS3_GAME itself.
pub fn dump_root(picked: &Path) -> PathBuf {
    if picked.file_name().map(|n| n.eq_ignore_ascii_case("PS3_GAME")).unwrap_or(false) {
        picked.parent().map(|p| p.to_path_buf()).unwrap_or_else(|| picked.to_path_buf())
    } else {
        picked.to_path_buf()
    }
}

/// Checks a dump for game g: Ok(()) or why not.
pub fn check_dump(g: &Game, root: &Path) -> Result<(), String> {
    let game_dir = root.join("PS3_GAME");
    let id = sfo_value(&game_dir.join("PARAM.SFO"), "TITLE_ID").ok_or("no PS3_GAME/PARAM.SFO in this folder")?;
    if id != g.title_id {
        return Err(format!("this is {id}, {} needs {}", g.title, g.title_id));
    }
    if !game_dir.join("USRDIR").join("EBOOT.BIN").is_file() {
        return Err("PS3_GAME/USRDIR/EBOOT.BIN is missing".into());
    }
    Ok(())
}

fn no_window(c: &mut Command) -> &mut Command {
    use std::os::windows::process::CommandExt;
    c.creation_flags(0x0800_0000) // CREATE_NO_WINDOW
}

fn runs(program: &str, args: &[&str]) -> Option<String> {
    let out = no_window(Command::new(program).args(args)).output().ok()?;
    if !out.status.success() {
        return None;
    }
    Some(String::from_utf8_lossy(&out.stdout).lines().next().unwrap_or("").trim().to_string())
}

/// Visual Studio Build Tools with its Clang compiler (Microsoft-signed, so Smart App Control lets it run).
fn build_tools() -> Prereq {
    let vswhere = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe";
    let found = runs(vswhere, &["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
                                "-property", "installationPath"])
        .filter(|s| !s.is_empty());
    Prereq {
        name: "Visual Studio Build Tools (C++ Clang)".into(),
        ok: found.is_some(),
        detail: found.unwrap_or_else(|| "not found: install the C++ Clang tools for Windows".into()),
        action: Some("https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio".into()),
    }
}

pub fn prereqs(cfg: &Config) -> Vec<Prereq> {
    let cmake = runs("cmake", &["--version"]);
    let python = runs("py", &["-3", "--version"]).or_else(|| runs("python", &["--version"]));
    let rpcs3 = cfg.rpcs3.clone().filter(|p| Path::new(p).is_file());
    let firmware = rpcs3.as_ref().map(|p| Path::new(p).parent().unwrap_or(Path::new("")).join("dev_flash").join("sys").is_dir());
    vec![
        build_tools(),
        Prereq {
            name: "CMake".into(),
            ok: cmake.is_some(),
            detail: cmake.unwrap_or_else(|| "not found on PATH".into()),
            action: Some("https://cmake.org/download/".into()),
        },
        Prereq {
            name: "Python 3".into(),
            ok: python.is_some(),
            detail: python.unwrap_or_else(|| "not found on PATH".into()),
            action: Some("https://www.python.org/downloads/".into()),
        },
        Prereq {
            name: "RPCS3 with firmware".into(),
            ok: rpcs3.is_some() && firmware == Some(true),
            detail: match (&rpcs3, firmware) {
                (None, _) => "choose rpcs3.exe: it decrypts the game once, with your firmware".into(),
                (Some(p), Some(false)) => format!("{p}: no firmware installed in it"),
                (Some(p), _) => p.clone(),
            },
            action: Some("pick-rpcs3".into()),
        },
    ]
}

pub fn overview(cfg: &Config) -> Overview {
    let build = cfg.build_dir();
    let games = GAMES
        .iter()
        .map(|g| {
            let dump = cfg.games.get(g.id).and_then(|c| c.dump.clone());
            let (status, detail) = if !g.supported {
                ("unsupported", "Coming later".to_string())
            } else {
                match &dump {
                    None => ("missing", format!("Choose your dump of {}", g.title_id)),
                    Some(d) => match check_dump(g, Path::new(d)) {
                        Err(why) => ("missing", why),
                        Ok(()) if build.join(g.exe).is_file() => ("built", "Ready to play".to_string()),
                        Ok(()) => ("ready", "Not built yet".to_string()),
                    },
                }
            };
            GameView {
                id: g.id.into(),
                title: g.title.into(),
                title_id: g.title_id.into(),
                supported: g.supported,
                status: status.into(),
                dump,
                detail,
            }
        })
        .collect();
    Overview { games, prereqs: prereqs(cfg), source: cfg.source_dir().display().to_string() }
}

pub fn command(program: &str) -> Command {
    let mut c = Command::new(program);
    no_window(&mut c);
    c
}
