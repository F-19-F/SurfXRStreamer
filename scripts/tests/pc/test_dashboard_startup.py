"""Compile the production tray start/drop methods against an inert fake Win32 API."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "build/reports/dashboard-startup-test"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / "scripts/transport"))
from source_fixture import method, production_source

source_file = ROOT / "surfxr/pc/surfxr-server/surfxr/dashboard/src/tray.rs"
source = production_source(source_file)
pieces = [source[source.index("#[derive(Default)]"):source.index("impl Events")],
          source[source.index("struct WindowState"):source.index('unsafe extern "system" fn window_proc')],
          source[source.index("pub enum Startup"):source.index("impl Tray")],
          "impl Tray {" + method(source, "fn start_named(") + "}", method(source, "impl Drop for Tray")]
prefix = r'''
#![allow(dead_code, non_snake_case, non_camel_case_types, unused_unsafe)]
use std::{ptr, sync::{Arc, Mutex, atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering}, mpsc}, thread};
type HANDLE = *mut std::ffi::c_void;
type HWND = HANDLE;
type HICON = HANDLE;
#[derive(Default)] struct Context;
mod panel { pub struct StartupGuard; }
mod i18n { #[derive(Default, Clone, Copy)] pub struct Language; pub fn language()->Language{Language} pub fn set_language(_:Language){} }
const ERROR_ALREADY_EXISTS:u32=183;
const IMAGE_ICON:u32=1; const LR_DEFAULTCOLOR:u32=0; const IDI_APPLICATION:*const u16=1 as *const u16;
const NIF_MESSAGE:u32=1; const NIF_ICON:u32=2; const NIF_TIP:u32=4; const CALLBACK:u32=1025;
const GWLP_USERDATA:i32=-21; const NIM_ADD:u32=0; const SHOW:u32=1026; const WM_CLOSE:u32=16;
static REFS:AtomicUsize=AtomicUsize::new(0);
static LAST_ERROR:AtomicU32=AtomicU32::new(0);
static MUTEX_FAILURE:AtomicBool=AtomicBool::new(false);
static WINDOW_FAILURE:AtomicBool=AtomicBool::new(false);
static ICON_FAILURE:AtomicBool=AtomicBool::new(false);
static WINDOW:AtomicBool=AtomicBool::new(false);
static QUIT:AtomicBool=AtomicBool::new(false);
static ACTIVATIONS:AtomicUsize=AtomicUsize::new(0);
fn wide(value:&str)->Vec<u16>{value.encode_utf16().chain(Some(0)).collect()}
fn CreateMutexW(_: *const (),_:i32,_:*const u16)->HANDLE {
 if MUTEX_FAILURE.load(Ordering::SeqCst){LAST_ERROR.store(5,Ordering::SeqCst);return ptr::null_mut()}
 let previous=REFS.fetch_add(1,Ordering::SeqCst);
 LAST_ERROR.store(if previous>0{ERROR_ALREADY_EXISTS}else{0},Ordering::SeqCst);
 1 as HANDLE
}
fn GetLastError()->u32{LAST_ERROR.load(Ordering::SeqCst)}
fn CloseHandle(_:HANDLE)->i32{REFS.fetch_sub(1,Ordering::SeqCst);1}
fn FindWindowW(_:*const u16,_:*const u16)->HWND{if WINDOW.load(Ordering::SeqCst){2 as HWND}else{ptr::null_mut()}}
fn GetModuleHandleW(_:*const u16)->HANDLE{3 as HANDLE}
#[derive(Default)] struct WNDCLASSW {
 lpfnWndProc:Option<unsafe extern "system" fn(HWND,u32,usize,isize)->isize>, hInstance:HANDLE, lpszClassName:*const u16,
}
fn RegisterClassW(_:&WNDCLASSW)->u16{1}
fn CreateWindowExW(_:u32,_:*const u16,_:*const u16,_:u32,_:i32,_:i32,_:i32,_:i32,_:HWND,_:HANDLE,_:HANDLE,_:*const ())->HWND{
 if WINDOW_FAILURE.load(Ordering::SeqCst){return ptr::null_mut()}
 QUIT.store(false,Ordering::SeqCst); WINDOW.store(true,Ordering::SeqCst); 2 as HWND
}
fn LoadImageW(_:HANDLE,_:*const u16,_:u32,_:i32,_:i32,_:u32)->HANDLE{4 as HANDLE}
fn LoadIconW(_:HANDLE,_:*const u16)->HICON{4 as HICON}
struct NOTIFYICONDATAW {cbSize:u32,hWnd:HWND,uID:u32,uFlags:u32,uCallbackMessage:u32,hIcon:HICON,szTip:[u16;128]}
impl Default for NOTIFYICONDATAW {fn default()->Self{Self{cbSize:0,hWnd:ptr::null_mut(),uID:0,uFlags:0,uCallbackMessage:0,hIcon:ptr::null_mut(),szTip:[0;128]}}}
fn RegisterWindowMessageW(_:*const u16)->u32{2000}
fn SetWindowLongPtrW(_:HWND,_:i32,_:isize)->isize{0}
fn Shell_NotifyIconW(_:u32,_:&NOTIFYICONDATAW)->i32{if ICON_FAILURE.load(Ordering::SeqCst){0}else{1}}
fn DestroyWindow(_:HWND)->i32{WINDOW.store(false,Ordering::SeqCst);QUIT.store(true,Ordering::SeqCst);1}
fn PostMessageW(hwnd:HWND,message:u32,_:usize,_:isize)->i32{
 if message==WM_CLOSE{DestroyWindow(hwnd);}else if message==SHOW{ACTIVATIONS.fetch_add(1,Ordering::SeqCst);} 1
}
#[derive(Default)] struct MSG;
fn GetMessageW(_:&mut MSG,_:HWND,_:u32,_:u32)->i32{
 while !QUIT.load(Ordering::SeqCst){thread::sleep(std::time::Duration::from_millis(1));} 0
}
fn TranslateMessage(_:&MSG)->i32{1}
fn DispatchMessageW(_:&MSG)->isize{0}
fn DestroyIcon(_:HICON)->i32{1}
fn UnregisterClassW(_:*const u16,_:HANDLE)->i32{1}
unsafe extern "system" fn window_proc(_:HWND,_:u32,_:usize,_:isize)->isize{0}
'''
suffix = r'''
fn main(){
 let identity="SURFXR.SurfXR.Fake.No.OS.Objects";
 let Startup::New(instance,Some(owner))=Tray::start_named(identity,false).unwrap() else{panic!("positive setup failed")};
 assert_eq!(REFS.load(Ordering::SeqCst),1);
 assert!(matches!(Tray::start_named(identity,false).unwrap(),Startup::Existing));
 assert_eq!(ACTIVATIONS.load(Ordering::SeqCst),0);
 assert!(matches!(Tray::start_named(identity,true).unwrap(),Startup::Existing));
 assert_eq!(ACTIVATIONS.load(Ordering::SeqCst),1);
 drop(owner); assert_eq!(REFS.load(Ordering::SeqCst),1); assert!(!WINDOW.load(Ordering::SeqCst));
 // DataSources shutdown may continue after the tray is gone. Its instance
 // cannot be replaced until main has released the independent guard.
 assert!(matches!(Tray::start_named(identity,false).unwrap(),Startup::Existing));
 drop(instance); assert_eq!(REFS.load(Ordering::SeqCst),0);
 let Startup::New(restarted_instance,Some(restarted))=Tray::start_named(identity,false).unwrap() else{panic!("normal exit did not release singleton")};
 drop(restarted); drop(restarted_instance); assert_eq!(REFS.load(Ordering::SeqCst),0);
 for failure in ["window", "icon"] {
  WINDOW_FAILURE.store(failure=="window",Ordering::SeqCst);
  ICON_FAILURE.store(failure=="icon",Ordering::SeqCst);
  let Startup::New(instance,None)=Tray::start_named(identity,false).unwrap() else{panic!("expected visible fallback")};
  // The real main keeps its visible fallback Dashboard running at this point.
  let references_while_first_fallback_lives=REFS.load(Ordering::SeqCst);
  let second=Tray::start_named(identity,false).unwrap();
  assert!(matches!(second,Startup::Existing));
  assert_eq!(references_while_first_fallback_lives,1);
  assert_eq!(REFS.load(Ordering::SeqCst),1);
  drop(instance); assert_eq!(REFS.load(Ordering::SeqCst),0);
  let Startup::New(restarted,None)=Tray::start_named(identity,false).unwrap() else{panic!("fallback exit did not release singleton")};
  drop(restarted); assert_eq!(REFS.load(Ordering::SeqCst),0);
  println!("{} failure: visible fallback retains singleton, duplicate rejected, exit permits reentry",failure);
 }
 MUTEX_FAILURE.store(true,Ordering::SeqCst);
 let result=Tray::start_named(identity,false);
 assert!(result.is_err()); assert_eq!(result.err().unwrap().raw_os_error(),Some(5));
 assert_eq!(REFS.load(Ordering::SeqCst),0);
 println!("PASS: mutex failure rejects startup; normal singleton/activation/drop and both creation fallbacks; no real Win32 calls");
}
'''
generated = OUT / "tray_fake_probe.rs"
generated.write_text(prefix + "\n".join(pieces) + suffix, encoding="utf-8")
binary = OUT / "tray_fake_probe.exe"
compile_result = subprocess.run(["rustc", "--edition=2024", str(generated), "-o", str(binary)], capture_output=True, text=True)
(OUT / "tray_fake_compile.log").write_text(compile_result.stdout + compile_result.stderr, encoding="utf-8")
if compile_result.returncode:
    raise SystemExit(compile_result.stderr)
result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
(OUT / "tray_fake_result.json").write_text(json.dumps({"source_sha256":hashlib.sha256(source_file.read_bytes()).hexdigest(),
 "production_methods_extracted":["Tray::start_named", "Tray::drop", "InstanceGuard::drop"], "platform":"inert fake Win32 API; no registry/service/tray objects", "exit_code":result.returncode, "stdout":result.stdout,"stderr":result.stderr},indent=2),encoding="utf-8")
print(result.stdout)
raise SystemExit(result.returncode)
