use surfxr_common::anyhow::{Result, bail, ensure};
use cpal::{Device, platform::DeviceInner};
use rodio::DeviceTrait;
use windows::{
    Win32::{
        Devices::FunctionDiscovery::PKEY_Device_FriendlyName,
        Media::Audio::{
            DEVICE_STATE_ACTIVE, Endpoints::IAudioEndpointVolume, IMMDevice, IMMDeviceEnumerator,
            MMDeviceEnumerator, eCapture, eRender,
        },
        System::Com::{self, CLSCTX_ALL, COINIT_MULTITHREADED, STGM_READ},
    },
    core::GUID,
};

fn get_windows_device(device: &Device) -> Result<IMMDevice> {
    let device_name = device.name()?;

    unsafe {
        // This will fail the second time is called, ignore the error
        Com::CoInitializeEx(None, COINIT_MULTITHREADED).ok().ok();

        let imm_device_enumerator: IMMDeviceEnumerator =
            Com::CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)?;

        let direction = if device.supports_output() {
            eRender
        } else {
            eCapture
        };
        let imm_device_collection =
            imm_device_enumerator.EnumAudioEndpoints(direction, DEVICE_STATE_ACTIVE)?;

        for i in 0..imm_device_collection.GetCount()? {
            let imm_device = imm_device_collection.Item(i)?;

            let imm_device_name = imm_device
                .OpenPropertyStore(STGM_READ)?
                .GetValue(&PKEY_Device_FriendlyName)?
                .to_string();

            if imm_device_name == device_name {
                return Ok(imm_device);
            }
        }

        bail!("No device found with specified name")
    }
}

pub fn get_windows_device_id(device: &Device) -> Result<String> {
    unsafe {
        let imm_device = get_windows_device(device)?;

        let id_str_ptr = imm_device.GetId()?;
        let id_str = id_str_ptr.to_string()?;
        Com::CoTaskMemFree(Some(id_str_ptr.0 as _));

        Ok(id_str)
    }
}

// Steam's capture endpoint maps scalar 1.0 to +30 dB, not unity. Keep local
// microphone gain meaningful and preserve attenuation/mute chosen by the user.
pub fn cap_steam_microphone_boost_windows(device: &Device) -> Result<Option<f32>> {
    if !device.name()?.to_lowercase().contains("steam streaming microphone") {
        return Ok(None);
    }
    unsafe {
        let endpoint = get_windows_device(device)?;
        let volume = endpoint.Activate::<IAudioEndpointVolume>(CLSCTX_ALL, None)?;
        let previous_db = volume.GetMasterVolumeLevel()?;
        ensure!(previous_db.is_finite(), "Invalid virtual microphone gain");
        if previous_db <= 0.0 {
            return Ok(None);
        }
        let (mut minimum, mut maximum, mut step) = (0.0, 0.0, 0.0);
        volume.GetVolumeRange(&mut minimum, &mut maximum, &mut step)?;
        ensure!(minimum <= 0.0 && maximum >= 0.0, "Virtual microphone lacks unity gain");
        volume.SetMasterVolumeLevel(0.0, &GUID::zeroed())?;
        ensure!(volume.GetMasterVolumeLevel()? <= 0.001, "Virtual microphone boost remains enabled");
        Ok(Some(previous_db))
    }
}

// device must be an output device
pub fn set_mute_windows_device(device: &Device, mute: bool) -> Result<()> {
    unsafe {
        let imm_device = get_windows_device(device)?;

        let endpoint_volume = imm_device.Activate::<IAudioEndpointVolume>(CLSCTX_ALL, None)?;

        endpoint_volume.SetMute(mute, &GUID::zeroed())?;
    }

    Ok(())
}

pub fn is_same_device(device1: &Device, device2: &Device) -> bool {
    let DeviceInner::Wasapi(dev1) = device1.as_inner();
    let DeviceInner::Wasapi(dev2) = device2.as_inner();

    dev1 == dev2
}
