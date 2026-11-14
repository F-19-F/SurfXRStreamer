//! One narrowly scoped elevated service worker per Dashboard lifetime.
use std::{fs::File, io::{Read, Write}, os::windows::io::{FromRawHandle, AsRawHandle},
    path::PathBuf, process::Command, ptr, sync::Mutex, time::{Duration, Instant}};
use windows_sys::Win32::{Foundation::*, Storage::FileSystem::*, System::{Pipes::*, Threading::*, SystemInformation::*},
    UI::{Shell::*, WindowsAndMessaging::SW_HIDE}};

static WORKER: Mutex<Option<Worker>> = Mutex::new(None);
struct Handle(HANDLE);
unsafe impl Send for Handle {}
impl Drop for Handle { fn drop(&mut self) { unsafe { CloseHandle(self.0); } } }
struct Worker { pipe: File, process: Handle }
fn wide(s: &str) -> Vec<u16> { s.encode_utf16().chain(Some(0)).collect() }
fn error() -> String { std::io::Error::last_os_error().to_string() }

// No caller-supplied executable, path, command line or service name crosses the pipe.
fn arguments(action: u8) -> Option<(&'static str, Vec<&'static str>)> {
    Some(match action {
        1 => ("daemon", vec!["--install-service", "--dashboard-managed"]),
        2 => ("daemon", vec!["--uninstall-service"]),
        3 => ("sc", vec!["start", "SurfXRDesktop"]),
        4 => ("sc", vec!["stop", "SurfXRDesktop"]),
        5 => ("sc", vec!["config", "SurfXRDesktop", "start=", "auto"]),
        6 => ("sc", vec!["config", "SurfXRDesktop", "start=", "demand"]),
        7 => ("sc", vec!["config", "SurfXRDesktop", "start=", "disabled"]),
        _ => return None,
    })
}
pub fn action(action: &str) -> Result<(), String> {
    let code = match action { "install" => 1, "uninstall" => 2, "start" => 3,
        "stop" => 4, "auto" => 5, "demand" => 6, "disabled" => 7,
        _ => return Err("Unknown service action".into()) };
    let mut worker = WORKER.lock().unwrap();
    dispatch(&mut worker, code, Worker::start)
}
fn dispatch(worker: &mut Option<Worker>, code: u8, start: impl FnOnce() -> Result<Worker, String>) -> Result<(), String> {
    if worker.is_none() { *worker = Some(start()?); }
    match worker.as_mut().unwrap().request(code) {
        Ok(0) => Ok(()),
        Ok(status) => Err(format!("Service operation failed (exit code {status})")),
        Err(error) => { *worker = None; Err(error) }
    }
}
pub fn shutdown() {
    // Do not delay Dashboard exit behind an in-flight service operation.
    // The worker's parent process handle also revokes it when the process exits.
    if let Ok(mut worker) = WORKER.try_lock() { worker.take(); }
}
impl Worker {
    fn start() -> Result<Self, String> {
        let name = format!(r"\\.\pipe\SurfXR.Service.{}.{:032x}", std::process::id(), rand::random::<u128>());
        let pipe = unsafe { CreateNamedPipeW(wide(&name).as_ptr(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64, 64, 0, ptr::null()) };
        if pipe == INVALID_HANDLE_VALUE { return Err(error()); }
        let pipe = unsafe { File::from_raw_handle(pipe) };
        let exe = wide(&std::env::current_exe().map_err(|e| e.to_string())?.to_string_lossy());
        let args = wide(&format!("--service-worker {} {}", std::process::id(), name));
        let verb = wide("runas");
        let mut launch: SHELLEXECUTEINFOW = unsafe { std::mem::zeroed() };
        launch.cbSize = std::mem::size_of_val(&launch) as u32;
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        launch.lpVerb = verb.as_ptr(); launch.lpFile = exe.as_ptr(); launch.lpParameters = args.as_ptr();
        launch.nShow = SW_HIDE;
        if unsafe { ShellExecuteExW(&mut launch) } == 0 { return Err(error()); }
        if launch.hProcess.is_null() { return Err("Elevated worker handle missing".into()); }
        let process = Handle(launch.hProcess);
        Self::connect(pipe, process)
    }
    fn connect(pipe: File, process: Handle) -> Result<Self, String> {
        let deadline = Instant::now() + Duration::from_secs(30);
        loop {
            let connected = unsafe { ConnectNamedPipe(pipe.as_raw_handle(), ptr::null_mut()) } != 0
                || unsafe { GetLastError() } == ERROR_PIPE_CONNECTED;
            if connected { break; }
            if Instant::now() >= deadline || unsafe { WaitForSingleObject(process.0, 0) } != WAIT_TIMEOUT {
                return Err("Elevated service worker did not connect".into());
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        let mut pid = 0;
        if unsafe { GetNamedPipeClientProcessId(pipe.as_raw_handle(), &mut pid) } == 0
            || pid != unsafe { GetProcessId(process.0) } {
            return Err("Elevated service worker identity mismatch".into());
        }
        let mode = PIPE_READMODE_BYTE | PIPE_WAIT;
        if unsafe { SetNamedPipeHandleState(pipe.as_raw_handle(), &mode, ptr::null(), ptr::null()) } == 0 {
            return Err(error());
        }
        Ok(Self { pipe, process })
    }
    fn request(&mut self, code: u8) -> Result<i32, String> {
        if unsafe { WaitForSingleObject(self.process.0, 0) } != WAIT_TIMEOUT {
            return Err("Elevated service worker exited. Retry to authorize a new worker.".into());
        }
        self.pipe.write_all(&[code]).map_err(|e| e.to_string())?;
        let mut response = [0; 4];
        self.pipe.read_exact(&mut response).map_err(|e| e.to_string())?;
        Ok(i32::from_le_bytes(response))
    }
}

fn same_executable(parent: HANDLE) -> bool {
    let mut path = vec![0; 32768]; let mut size = path.len() as u32;
    if unsafe { QueryFullProcessImageNameW(parent, 0, path.as_mut_ptr(), &mut size) } == 0 { return false; }
    let parent = PathBuf::from(String::from_utf16_lossy(&path[..size as usize]));
    std::env::current_exe().ok().and_then(|p| p.canonicalize().ok()).zip(parent.canonicalize().ok())
        .is_some_and(|(a,b)| a.to_string_lossy().eq_ignore_ascii_case(&b.to_string_lossy()))
}
pub fn run() -> Result<(), String> {
    let args: Vec<_> = std::env::args().collect();
    if args.len() != 4 || args[1] != "--service-worker" { return Err("Invalid service worker arguments".into()); }
    let pid: u32 = args[2].parse().map_err(|_| "Invalid Dashboard PID")?;
    let prefix = format!(r"\\.\pipe\SurfXR.Service.{pid}.");
    if !args[3].strip_prefix(&prefix).is_some_and(|v| v.len() == 32 && v.bytes().all(|b| b.is_ascii_hexdigit())) {
        return Err("Invalid service pipe".into());
    }
    let parent = Handle(unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, 0, pid) });
    if parent.0.is_null() || !same_executable(parent.0) { return Err("Dashboard identity mismatch".into()); }
    let handle = unsafe { CreateFileW(wide(&args[3]).as_ptr(), GENERIC_READ | GENERIC_WRITE, 0,
        ptr::null(), OPEN_EXISTING, 0, ptr::null_mut()) };
    if handle == INVALID_HANDLE_VALUE { return Err(error()); }
    let mut pipe = unsafe { File::from_raw_handle(handle) };
    let mut server = 0;
    if unsafe { GetNamedPipeServerProcessId(handle, &mut server) } == 0 || server != pid {
        return Err("Dashboard pipe owner mismatch".into());
    }
    // Kernel process handle prevents PID reuse. Parent exit revokes this worker even mid-operation.
    std::thread::spawn(move || {
        let _parent = parent;
        unsafe { WaitForSingleObject(_parent.0, INFINITE); }
        std::process::exit(0);
    });
    let mut code = [0];
    while pipe.read_exact(&mut code).is_ok() {
        let Some((program, args)) = arguments(code[0]) else { return Err("Invalid service opcode".into()); };
        let executable = if program == "daemon" {
            std::env::current_exe().map_err(|e| e.to_string())?.with_file_name("surfxr-daemon.exe")
        } else {
            let mut root = vec![0u16; 32768];
            let count = unsafe { GetSystemDirectoryW(root.as_mut_ptr(), root.len() as u32) };
            if count == 0 || count as usize >= root.len() { return Err(error()); }
            PathBuf::from(String::from_utf16_lossy(&root[..count as usize])).join("sc.exe")
        };
        use std::os::windows::process::CommandExt;
        let status = Command::new(executable).args(args).creation_flags(CREATE_NO_WINDOW)
            .status().map(|s| s.code().unwrap_or(-1)).unwrap_or(-1);
        pipe.write_all(&status.to_le_bytes()).map_err(|e| e.to_string())?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn private_pipe_reuses_permission_after_command_failure_and_revokes_on_disconnect() {
        let name = wide(&format!(r"\\.\pipe\SurfXR.Service.Test.{:032x}", rand::random::<u128>()));
        let handle = unsafe { CreateNamedPipeW(name.as_ptr(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64, 64, 0, ptr::null()) };
        assert_ne!(handle, INVALID_HANDLE_VALUE);
        let pipe = unsafe { File::from_raw_handle(handle) };
        let peer = std::thread::spawn(move || {
            let handle = unsafe { CreateFileW(name.as_ptr(), GENERIC_READ | GENERIC_WRITE, 0,
                ptr::null(), OPEN_EXISTING, 0, ptr::null_mut()) };
            assert_ne!(handle, INVALID_HANDLE_VALUE);
            let mut pipe = unsafe { File::from_raw_handle(handle) };
            let mut owner = 0;
            assert_ne!(unsafe { GetNamedPipeServerProcessId(handle, &mut owner) }, 0);
            assert_eq!(owner, std::process::id());
            for (expected, status) in [(1, 5i32), (3, 0)] {
                let mut code = [0]; pipe.read_exact(&mut code).unwrap();
                assert_eq!(code[0], expected);
                pipe.write_all(&status.to_le_bytes()).unwrap();
            }
            // Closing the isolated peer simulates worker loss; no SCM command is executed.
        });
        let process = Handle(unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, 0, std::process::id()) });
        assert!(same_executable(process.0));
        let mut worker = None;
        assert!(dispatch(&mut worker, 1, || Worker::connect(pipe, process)).is_err());
        assert!(worker.is_some()); // A failed install does not require another UAC.
        dispatch(&mut worker, 3, || panic!("must reuse authorization")).unwrap();
        peer.join().unwrap();
        assert!(dispatch(&mut worker, 4, || panic!("must not silently reauthorize")).is_err());
        assert!(worker.is_none());
        assert!(dispatch(&mut worker, 1, || Err("UAC cancelled".into())).is_err());
        assert!(worker.is_none());
    }
    #[test]
    fn service_commands_are_closed_and_fixed() {
        for byte in 0..=255 { assert_eq!(arguments(byte).is_some(), (1..=7).contains(&byte)); }
        assert_eq!(arguments(1).unwrap(), ("daemon", vec!["--install-service", "--dashboard-managed"]));
        assert_eq!(arguments(3).unwrap(), ("sc", vec!["start", "SurfXRDesktop"]));
    }
}
