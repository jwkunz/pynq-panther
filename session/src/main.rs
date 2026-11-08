//! panther-session - bring up the HDMI desktop on a PYNQ-Z2, or leave the board headless.
//!
//! Sequence (every step has a timeout; any failure ends in "stay headless", exit code 0, so boot is never blocked):
//!   1. read /etc/panther/panther.conf (enabled = false turns everything off)
//!   2. wait until the programmable logic is configured (the base overlay is loaded by bootpy.service)
//!   3. load the panther_drm kernel module and find the DRM card it creates
//!   4. write an Xorg config that points the modesetting driver at that card, start Xorg
//!   5. start the desktop session (fluxbox) as the desktop user; restart X a few times if it dies
use std::collections::HashMap;
use std::fs;
use std::io::Write;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::thread::sleep;
use std::time::{Duration, Instant};

const CONF: &str = "/etc/panther/panther.conf";
const RUN_DIR: &str = "/run/panther";

macro_rules! log {
    ($($a:tt)*) => {{ println!("panther-session: {}", format!($($a)*)); let _ = std::io::stdout().flush(); }};
}

struct Conf {
    enabled: bool,
    user: String,
    vt: u32,
    module: String,
    session: String,
    max_restarts: u32,
    pl_timeout: u64,
}

impl Conf {
    fn load(path: &str) -> Conf {
        let mut m: HashMap<String, String> = HashMap::new();
        if let Ok(text) = fs::read_to_string(path) {
            for line in text.lines() {
                let line = line.trim();
                if line.is_empty() || line.starts_with('#') {
                    continue;
                }
                if let Some((k, v)) = line.split_once('=') {
                    m.insert(k.trim().to_string(), v.trim().trim_matches('"').to_string());
                }
            }
        }
        let get = |k: &str, d: &str| m.get(k).cloned().unwrap_or_else(|| d.to_string());
        Conf {
            enabled: !matches!(get("enabled", "true").as_str(), "false" | "0" | "no"),
            user: get("user", "xilinx"),
            vt: get("vt", "7").parse().unwrap_or(7),
            module: get("module", "/usr/local/lib/panther/panther_drm.ko"),
            session: get("session", "/usr/local/lib/panther/session.sh"),
            max_restarts: get("max_restarts", "3").parse().unwrap_or(3),
            pl_timeout: get("pl_timeout", "120").parse().unwrap_or(120),
        }
    }
}

fn run_ok(cmd: &mut Command) -> Result<(), String> {
    let out = cmd.output().map_err(|e| format!("{:?}: {}", cmd, e))?;
    if out.status.success() {
        Ok(())
    } else {
        Err(format!("{:?} failed: {}", cmd, String::from_utf8_lossy(&out.stderr).trim()))
    }
}

/// The FPGA manager reports "operating" once a bitstream is loaded.
fn pl_operating() -> bool {
    fs::read_to_string("/sys/class/fpga_manager/fpga0/state").map(|s| s.trim() == "operating").unwrap_or(false)
}

fn wait_for_pl(timeout: u64) -> bool {
    let t0 = Instant::now();
    while t0.elapsed().as_secs() < timeout {
        if pl_operating() {
            return true;
        }
        sleep(Duration::from_secs(1));
    }
    false
}

fn module_loaded(name: &str) -> bool {
    fs::read_to_string("/proc/modules").map(|s| s.lines().any(|l| l.split_whitespace().next() == Some(name))).unwrap_or(false)
}

/// Find /dev/dri/cardN whose driver is panther-drm.
fn find_card() -> Option<PathBuf> {
    let rd = fs::read_dir("/sys/class/drm").ok()?;
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        if !(name.starts_with("card") && name[4..].chars().all(|c| c.is_ascii_digit())) {
            continue;
        }
        let uevent = fs::read_to_string(e.path().join("device/uevent")).unwrap_or_default();
        if uevent.lines().any(|l| l == "DRIVER=panther-drm") {
            return Some(PathBuf::from(format!("/dev/dri/{}", name)));
        }
    }
    None
}

fn load_driver(c: &Conf) -> Result<PathBuf, String> {
    if !module_loaded("panther_drm") {
        // prefer a depmod-installed copy, fall back to the explicit file
        if run_ok(Command::new("modprobe").arg("panther_drm")).is_err() {
            run_ok(Command::new("insmod").arg(&c.module))?;
        }
    }
    let t0 = Instant::now();
    while t0.elapsed().as_secs() < 10 {
        if let Some(card) = find_card() {
            return Ok(card);
        }
        sleep(Duration::from_millis(200));
    }
    Err("panther_drm loaded but no DRM card appeared (probe failed? see dmesg)".into())
}

fn write_xorg_conf(card: &Path) -> Result<PathBuf, String> {
    fs::create_dir_all(RUN_DIR).map_err(|e| e.to_string())?;
    let path = Path::new(RUN_DIR).join("xorg.conf");
    // Software rendering only (no GL on this board). ShadowFB keeps X from reading the uncached scan-out buffer (reads
    // run at ~80 MB/s, writes at ~490 MB/s).
    let conf = format!(
        r#"Section "ServerFlags"
    Option "AutoAddDevices" "true"
    Option "DontZap" "true"
EndSection
Section "Device"
    Identifier "panther"
    Driver "modesetting"
    Option "kmsdev" "{card}"
    Option "AccelMethod" "none"
    Option "ShadowFB" "true"
    Option "SWcursor" "true"
EndSection
Section "Monitor"
    Identifier "hdmi"
EndSection
Section "Screen"
    Identifier "screen"
    Device "panther"
    Monitor "hdmi"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
    EndSubSection
EndSection
Section "ServerLayout"
    Identifier "layout"
    Screen "screen"
EndSection
"#,
        card = card.display()
    );
    fs::write(&path, conf).map_err(|e| e.to_string())?;
    Ok(path)
}

fn user_ids(user: &str) -> Option<(u32, u32, String)> {
    let passwd = fs::read_to_string("/etc/passwd").ok()?;
    for l in passwd.lines() {
        let f: Vec<&str> = l.split(':').collect();
        if f.len() >= 6 && f[0] == user {
            return Some((f[2].parse().ok()?, f[3].parse().ok()?, f[5].to_string()));
        }
    }
    None
}

fn start_xorg(c: &Conf, conf: &Path) -> Result<Child, String> {
    let _ = fs::remove_file("/tmp/.X11-unix/X0");
    let child = Command::new("/usr/lib/xorg/Xorg")
        .args([":0", &format!("vt{}", c.vt), "-nolisten", "tcp", "-noreset", "-config"])
        .arg(conf)
        .args(["-logfile", &format!("{}/Xorg.log", RUN_DIR)])
        .stdin(Stdio::null())
        .spawn()
        .map_err(|e| format!("cannot start Xorg: {}", e))?;
    Ok(child)
}

fn wait_for_x(x: &mut Child, timeout: u64) -> Result<(), String> {
    let t0 = Instant::now();
    while t0.elapsed().as_secs() < timeout {
        if let Ok(Some(st)) = x.try_wait() {
            return Err(format!("Xorg exited early ({}); see {}/Xorg.log", st, RUN_DIR));
        }
        if Path::new("/tmp/.X11-unix/X0").exists() {
            return Ok(());
        }
        sleep(Duration::from_millis(250));
    }
    let _ = x.kill();
    Err("timed out waiting for Xorg".into())
}

fn start_session(c: &Conf) -> Result<Child, String> {
    let (uid, gid, home) = user_ids(&c.user).ok_or_else(|| format!("no such user {}", c.user))?;
    Command::new(&c.session)
        .uid(uid)
        .gid(gid)
        .current_dir(&home)
        .env_clear()
        .env("DISPLAY", ":0")
        .env("HOME", &home)
        .env("USER", &c.user)
        .env("LOGNAME", &c.user)
        .env("PATH", "/usr/local/bin:/usr/bin:/bin")
        .env("XDG_RUNTIME_DIR", RUN_DIR)
        .stdin(Stdio::null())
        .spawn()
        .map_err(|e| format!("cannot start session {}: {}", c.session, e))
}

fn main() {
    let c = Conf::load(CONF);
    if !c.enabled {
        log!("disabled in {}; staying headless", CONF);
        return;
    }
    log!("waiting for the PL (base overlay), up to {} s", c.pl_timeout);
    if !wait_for_pl(c.pl_timeout) {
        log!("PL never became operational; staying headless");
        return;
    }
    // bootpy.service has finished before we start (unit ordering), so the PL will not be reprogrammed under us.
    let card = match load_driver(&c) {
        Ok(card) => card,
        Err(e) => {
            log!("{}; staying headless", e);
            return;
        }
    };
    log!("display driver ready on {}", card.display());
    let conf = match write_xorg_conf(&card) {
        Ok(p) => p,
        Err(e) => {
            log!("cannot write Xorg config: {}; staying headless", e);
            return;
        }
    };
    let _ = fs::set_permissions(RUN_DIR, std::os::unix::fs::PermissionsExt::from_mode(0o755));
    if let Some((uid, gid, _)) = user_ids(&c.user) {
        let _ = std::os::unix::fs::chown(RUN_DIR, Some(uid), Some(gid));
    }

    let mut failures = 0;
    while failures <= c.max_restarts {
        let started = Instant::now();
        let mut x = match start_xorg(&c, &conf) {
            Ok(x) => x,
            Err(e) => {
                log!("{}", e);
                failures += 1;
                continue;
            }
        };
        if let Err(e) = wait_for_x(&mut x, 30) {
            log!("{}", e);
            failures += 1;
            sleep(Duration::from_secs(2));
            continue;
        }
        log!("X is up; starting the session as {}", c.user);
        let mut s = start_session(&c).ok();
        if s.is_none() {
            log!("session failed to start; X stays up without clients");
        }
        // supervise: if X dies, take the session down with it
        let st = x.wait();
        log!("Xorg exited: {:?}", st);
        if let Some(s) = s.as_mut() {
            let _ = s.kill();
            let _ = s.wait();
        }
        // a long healthy run resets the failure budget
        if started.elapsed().as_secs() > 120 {
            failures = 0;
        } else {
            failures += 1;
        }
        sleep(Duration::from_secs(2));
    }
    log!("giving up after repeated failures; the board stays headless");
}
