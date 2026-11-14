//! Boot starts the existing SYSTEM service; logon starts its Dashboard admission owner.
use std::ptr;
use windows_sys::Win32::{
    Foundation::*,
    System::{Registry::*, Services::*},
};

const RUN_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const RUN_NAME: &str = "SurfXR";
fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}
fn command() -> Result<String, String> {
    std::env::current_exe()
        .map(|path| format!("\"{}\" --tray", path.display()))
        .map_err(|e| e.to_string())
}

pub fn login_enabled() -> bool {
    let mut buffer = vec![0u16; 32768];
    let mut size = (buffer.len() * 2) as u32;
    let result = unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER,
            wide(RUN_KEY).as_ptr(),
            wide(RUN_NAME).as_ptr(),
            RRF_RT_REG_SZ,
            ptr::null_mut(),
            buffer.as_mut_ptr().cast(),
            &mut size,
        )
    };
    result == ERROR_SUCCESS
        && command().is_ok_and(|expected| {
            String::from_utf16_lossy(&buffer[..(size as usize / 2).saturating_sub(1)]) == expected
        })
}

pub fn set_login(enabled: bool) -> Result<(), String> {
    let data = if enabled {
        Some(wide(&command()?))
    } else {
        None
    };
    let mut key = ptr::null_mut();
    let result = unsafe {
        RegCreateKeyExW(
            HKEY_CURRENT_USER,
            wide(RUN_KEY).as_ptr(),
            0,
            ptr::null(),
            0,
            KEY_SET_VALUE,
            ptr::null(),
            &mut key,
            ptr::null_mut(),
        )
    };
    if result != ERROR_SUCCESS {
        return Err(format!("Logon registration: Windows error {result}"));
    }
    let result = unsafe {
        let result = if let Some(data) = data {
            RegSetValueExW(
                key,
                wide(RUN_NAME).as_ptr(),
                0,
                REG_SZ,
                data.as_ptr().cast(),
                (data.len() * 2) as u32,
            )
        } else {
            RegDeleteValueW(key, wide(RUN_NAME).as_ptr())
        };
        RegCloseKey(key);
        result
    };
    if result == ERROR_SUCCESS || (!enabled && result == ERROR_FILE_NOT_FOUND) {
        Ok(())
    } else {
        Err(format!("Logon registration: Windows error {result}"))
    }
}

pub fn automatic() -> Option<bool> {
    service_start_type().map(|value| value == SERVICE_AUTO_START)
}

fn service_start_type() -> Option<u32> {
    unsafe {
        let manager = OpenSCManagerW(ptr::null(), ptr::null(), SC_MANAGER_CONNECT);
        if manager.is_null() {
            return None;
        }
        let service = OpenServiceW(
            manager,
            wide("SurfXRDesktop").as_ptr(),
            SERVICE_QUERY_CONFIG,
        );
        CloseServiceHandle(manager);
        if service.is_null() {
            return None;
        }
        let mut needed = 0;
        QueryServiceConfigW(service, ptr::null_mut(), 0, &mut needed);
        // Aligned storage, with the size returned by SCM (maximum 8 KiB).
        let mut storage = vec![0usize; (needed as usize).div_ceil(std::mem::size_of::<usize>())];
        let config = storage.as_mut_ptr().cast::<QUERY_SERVICE_CONFIGW>();
        let ok = needed > 0 && QueryServiceConfigW(service, config, needed, &mut needed) != 0;
        let result = ok.then(|| (*config).dwStartType);
        CloseServiceHandle(service);
        result
    }
}

fn configure_service(start_type: u32) -> Result<(), String> {
    let mode = match start_type {
        SERVICE_AUTO_START => "auto",
        SERVICE_DEMAND_START => "demand",
        SERVICE_DISABLED => "disabled",
        _ => return Err("Unsupported desktop service startup type".into()),
    };
    crate::service_elevation::action(mode)
}

pub fn set_automatic(enabled: bool) -> Result<(), String> {
    let previous = service_start_type().ok_or("Cannot query desktop service startup")?;
    apply_startup(enabled, previous, configure_service, set_login)?;
    if automatic() != Some(enabled) || login_enabled() != enabled {
        return Err("Startup registration could not be verified".into());
    }
    Ok(())
}

fn apply_startup(
    enabled: bool,
    previous: u32,
    mut service: impl FnMut(u32) -> Result<(), String>,
    mut login: impl FnMut(bool) -> Result<(), String>,
) -> Result<(), String> {
    service(if enabled {
        SERVICE_AUTO_START
    } else {
        SERVICE_DEMAND_START
    })?;
    if let Err(error) = login(enabled) {
        let rollback = service(previous);
        return Err(format!("{error}; service rollback: {rollback:?}"));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    #[test]
    fn startup_changes_both_owners_and_cancel_does_not_register_logon() {
        let calls = RefCell::new(vec![]);
        for enabled in [true, false] {
            apply_startup(
                enabled,
                if enabled {
                    SERVICE_DEMAND_START
                } else {
                    SERVICE_AUTO_START
                },
                |value| {
                    calls
                        .borrow_mut()
                        .push(("service", value == SERVICE_AUTO_START));
                    Ok(())
                },
                |value| {
                    calls.borrow_mut().push(("login", value));
                    Ok(())
                },
            )
            .unwrap();
        }
        assert_eq!(
            *calls.borrow(),
            [
                ("service", true),
                ("login", true),
                ("service", false),
                ("login", false)
            ]
        );
        calls.borrow_mut().clear();
        assert!(
            apply_startup(
                true,
                SERVICE_DEMAND_START,
                |_| Err("UAC cancelled".into()),
                |value| {
                    calls.borrow_mut().push(("login", value));
                    Ok(())
                }
            )
            .is_err()
        );
        assert!(calls.borrow().is_empty());
    }
    #[test]
    fn failed_logon_registration_restores_previous_service_startup() {
        let mut service_changes = vec![];
        assert!(
            apply_startup(
                true,
                SERVICE_DISABLED,
                |value| {
                    service_changes.push(value);
                    Ok(())
                },
                |_| Err("Registry access denied".into())
            )
            .is_err()
        );
        assert_eq!(service_changes, [SERVICE_AUTO_START, SERVICE_DISABLED]);
    }
}
