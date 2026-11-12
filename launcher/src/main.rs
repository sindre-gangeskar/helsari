// Helsari launcher: two game cards, a Play button. A game that is not built yet asks for the
// player's own dump and builds the port from it, with progress; nothing from a game ships with it.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod config;
mod games;
mod pipeline;

use config::{Config, GameConfig};
use games::Overview;
use std::sync::Mutex;
use tauri::{AppHandle, State};

struct Launcher {
    cfg: Mutex<Config>,
    building: Mutex<Option<String>>,
}

#[tauri::command]
fn overview(l: State<Launcher>) -> Overview {
    games::overview(&l.cfg.lock().unwrap())
}

#[tauri::command]
fn building(l: State<Launcher>) -> Option<String> {
    l.building.lock().unwrap().clone()
}

#[tauri::command]
fn pick_dump(l: State<Launcher>, game: String) -> Result<Overview, String> {
    let g = games::game(&game).ok_or("unknown game")?;
    let picked = rfd::FileDialog::new().set_title(format!("Your dump of {} (the folder with PS3_GAME)", g.title_id)).pick_folder();
    let Some(picked) = picked else { return Ok(games::overview(&l.cfg.lock().unwrap())) };
    let root = games::dump_root(&picked);
    games::check_dump(g, &root)?;
    let mut cfg = l.cfg.lock().unwrap();
    cfg.games.entry(game).or_insert_with(GameConfig::default).dump = Some(root.display().to_string());
    cfg.save()?;
    Ok(games::overview(&cfg))
}

/// The game's own menu artwork and title from the player's dump, if one is chosen for it.
#[tauri::command]
fn art(l: State<Launcher>, game: String) -> Option<games::Art> {
    let dump = l.cfg.lock().unwrap().games.get(&game).and_then(|c| c.dump.clone())?;
    let root = std::path::PathBuf::from(dump);
    root.join("PS3_GAME").is_dir().then(|| games::art(&root))
}

#[tauri::command]
fn pick_rpcs3(l: State<Launcher>) -> Result<Overview, String> {
    let picked = rfd::FileDialog::new().set_title("rpcs3.exe").add_filter("RPCS3", &["exe"]).pick_file();
    let mut cfg = l.cfg.lock().unwrap();
    if let Some(p) = picked {
        cfg.rpcs3 = Some(p.display().to_string());
        cfg.save()?;
    }
    Ok(games::overview(&cfg))
}

/// Starts building a game; progress arrives as "build-progress" events, the end as "build-finished".
#[tauri::command]
fn build(app: AppHandle, l: State<Launcher>, game: String) -> Result<(), String> {
    let g = games::game(&game).filter(|g| g.supported).ok_or("this game is not supported yet")?;
    {
        let mut b = l.building.lock().unwrap();
        if b.is_some() {
            return Err("a build is already running".into());
        }
        *b = Some(game.clone());
    }
    let cfg = l.cfg.lock().unwrap().clone();
    std::thread::spawn(move || {
        pipeline::build(app.clone(), cfg, g);
        use tauri::Manager;
        *app.state::<Launcher>().building.lock().unwrap() = None;
    });
    Ok(())
}

#[tauri::command]
fn play(l: State<Launcher>, game: String) -> Result<(), String> {
    let g = games::game(&game).filter(|g| g.supported).ok_or("this game is not supported yet")?;
    let build = l.cfg.lock().unwrap().build_dir();
    let exe = build.join(g.exe);
    if !exe.is_file() {
        return Err(format!("{} is not built yet", g.title));
    }
    std::process::Command::new(&exe).current_dir(&build).spawn().map_err(|e| format!("cannot start {}: {e}", exe.display()))?;
    Ok(())
}

#[tauri::command]
fn open_link(url: String) -> Result<(), String> {
    if !url.starts_with("https://") {
        return Err("not a web link".into());
    }
    use std::os::windows::process::CommandExt;
    std::process::Command::new("rundll32")
        .args(["url.dll,FileProtocolHandler", &url])
        .creation_flags(0x0800_0000)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

fn main() {
    tauri::Builder::default()
        .manage(Launcher { cfg: Mutex::new(Config::load()), building: Mutex::new(None) })
        .invoke_handler(tauri::generate_handler![overview, building, pick_dump, pick_rpcs3, art, build, play, open_link])
        .run(tauri::generate_context!())
        .expect("the launcher window could not start");
}
