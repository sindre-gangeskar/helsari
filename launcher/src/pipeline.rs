//! Building a game from the player's dump: configure, then lift and compile (one CMake build: the
//! recompiler runs as its first steps), reported to the window as progress events.

use crate::config::Config;
use crate::games::{self, Game};
use serde::Serialize;
use std::io::{BufRead, BufReader};
use std::path::Path;
use std::process::Stdio;
use std::sync::mpsc;
use tauri::{AppHandle, Emitter};

#[derive(Serialize, Clone)]
pub struct Progress {
    pub game: String,
    pub stage: String,
    /// 0 to 100.
    pub percent: f32,
    /// The latest line of output, for the curious.
    pub line: String,
}

#[derive(Serialize, Clone)]
pub struct Finished {
    pub game: String,
    pub ok: bool,
    pub message: String,
    /// The last lines of output, when it failed.
    pub log: Vec<String>,
}

struct Reporter<'a> {
    app: &'a AppHandle,
    game: &'a str,
    tail: Vec<String>,
}

impl Reporter<'_> {
    fn send(&self, stage: &str, percent: f32, line: &str) {
        let _ = self.app.emit("build-progress", Progress { game: self.game.into(), stage: stage.into(), percent, line: line.into() });
    }
    fn keep(&mut self, line: &str) {
        self.tail.push(line.to_string());
        if self.tail.len() > 40 {
            self.tail.remove(0);
        }
    }
}

/// "[12/345] ..." from ninja: done and total steps.
fn ninja_step(line: &str) -> Option<(u32, u32)> {
    let rest = line.strip_prefix('[')?;
    let (n, rest) = rest.split_once('/')?;
    let (t, _) = rest.split_once(']')?;
    Some((n.trim().parse().ok()?, t.trim().parse().ok()?))
}

/// Runs a command, passing each output line (stdout and stderr) to on_line. Ok if it exited with 0.
fn run(cmd: &mut std::process::Command, mut on_line: impl FnMut(&str)) -> Result<(), String> {
    let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("cannot start: {e}"))?;
    let (tx, rx) = mpsc::channel::<String>();
    let readers: Vec<_> = [child.stdout.take().map(|s| Box::new(s) as Box<dyn std::io::Read + Send>),
                           child.stderr.take().map(|s| Box::new(s) as Box<dyn std::io::Read + Send>)]
        .into_iter()
        .flatten()
        .map(|r| {
            let tx = tx.clone();
            std::thread::spawn(move || {
                for line in BufReader::new(r).lines().map_while(Result::ok) {
                    let _ = tx.send(line);
                }
            })
        })
        .collect();
    drop(tx);
    for line in rx {
        on_line(&line);
    }
    for r in readers {
        let _ = r.join();
    }
    let status = child.wait().map_err(|e| e.to_string())?;
    if status.success() { Ok(()) } else { Err(format!("exited with {status}")) }
}

pub fn build(app: AppHandle, cfg: Config, g: &'static Game) {
    let mut r = Reporter { app: &app, game: g.id, tail: Vec::new() };
    let result = steps(&mut r, &cfg, g);
    let finished = match result {
        Ok(()) => Finished { game: g.id.into(), ok: true, message: format!("{} is ready to play", g.title), log: vec![] },
        Err(e) => Finished { game: g.id.into(), ok: false, message: e, log: r.tail.clone() },
    };
    let _ = app.emit("build-finished", finished);
}

fn steps(r: &mut Reporter, cfg: &Config, g: &Game) -> Result<(), String> {
    r.send("Checking your game", 1.0, "");
    let dump = cfg.games.get(g.id).and_then(|c| c.dump.clone()).ok_or("choose your game dump first")?;
    games::check_dump(g, Path::new(&dump))?;
    let missing: Vec<String> = games::prereqs(cfg).into_iter().filter(|p| !p.ok).map(|p| p.name).collect();
    if !missing.is_empty() {
        return Err(format!("missing: {}", missing.join(", ")));
    }
    let source = cfg.source_dir();
    let build = cfg.build_dir();
    if !source.join("CMakeLists.txt").is_file() {
        return Err(format!("no Helsari source tree at {}", source.display()));
    }

    // Configure: the dump, and RPCS3 (which also locates the installed game update).
    r.send("Preparing the build", 3.0, "");
    let mut configure = games::command("cmake");
    configure.arg("-S").arg(&source).arg("-B").arg(&build);
    if !build.join("CMakeCache.txt").is_file() {
        configure.args(["-G", "Ninja"]);
    }
    configure.arg(format!("-DHELSARI_DUMP={dump}"));
    if let Some(rpcs3) = &cfg.rpcs3 {
        configure.arg(format!("-DRPCS3_EXE={rpcs3}"));
    }
    run(&mut configure, |line| {
        r.keep(line);
        r.send("Preparing the build", 5.0, line);
    })
    .map_err(|e| format!("preparing the build failed ({e})"))?;

    // Build: the recompiler lifts the game's code first, then everything is compiled.
    let mut stage = "Lifting the game's code";
    let mut percent = 6.0;
    let mut build_cmd = games::command("cmake");
    build_cmd.arg("--build").arg(&build);
    run(&mut build_cmd, |line| {
        r.keep(line);
        if let Some((n, t)) = ninja_step(line) {
            if t > 0 {
                percent = 8.0 + 92.0 * n as f32 / t as f32;
            }
            stage = if line.contains("Linking") { "Linking" } else { "Compiling" };
        } else if line.starts_with("[recomp]") {
            stage = "Lifting the game's code";
        }
        r.send(stage, percent, line);
    })
    .map_err(|e| format!("the build failed ({e})"))?;

    if !build.join(g.exe).is_file() {
        return Err(format!("the build finished but {} is missing", g.exe));
    }
    r.send("Done", 100.0, "");
    Ok(())
}
