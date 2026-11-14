# Steam Frame devices and controller input

## Runtime IPD and desktop optics (2026-10-18)

`Hmd::SetViewParams` now publishes the IPD derived from the same eye transforms
to `Prop_UserIpdMeters_Float`, `steamvr.ipd`, and `VREvent_IpdChanged`, after
updating the display transforms. Previously those reporting paths stayed at
the 63 mm initialization value even while rendering used the client's IPD.
The default remains until valid views arrive; activation no longer sends a
fabricated IPD change event. Private `HmdIpd` skips dummy/invalid eye separation,
deduplicates identical values and <=1 micrometre noise, and only caches
successful property/settings writes. It does not emit lens-distortion events.

Desktop tracking clients advertise `frame_desktop_optics=1`. Only those clients
receive `frame_desktop_optics_v1`; after acknowledgment they send the existing
v21 `LocalViewParams` packet over the same control connection. The desktop
owner forwards it to the existing driver event queue only while **Keep SteamVR
tracking on desktop** is enabled, without creating video/audio owners. Old
clients receive no new acknowledgment; new clients send no optics to old
servers. Both Frame modes send >1 mm IPD / >0.001 rad FOV changes immediately.
Fine IPD changes require a one-sided trend (three 0.02 mm observation steps,
at least 0.1 mm net travel), or a >=0.1 mm step held for 300 ms. Accepted fine
movement publishes at most every 200 ms, with a 0.05 mm sending noise floor and
a final update before leaving follow mode after 400 ms idle. Reversals rebuild
direction evidence; purely one-sided mechanical drift is indistinguishable
from manual adjustment using IPD alone. Comparisons always use the last
transmitted value so continuous slow adjustment still advances. Reconnect, resume and
origin changes resend once; stable tracking does not produce optical packets.

Offline checks: `cargo test -p surfxr_server_core --lib`,
`cargo build -p surfxr_server_openvr --release`, and the real `HmdIpd` with fake
OpenVR interfaces:

```sh
g++ -std=c++17 -I openvr/headers surfxr/server_openvr/cpp/tools/hmd_ipd_test.cpp -o /tmp/hmd-ipd-test
/tmp/hmd-ipd-test
```

The paired Frame `surfxr-wire` / `surfxr-loopback` checks cover negotiation,
initial/accumulated IPD/FOV/epoch changes, invalid optics, disable/resume and
unchanged media ownership. Builds and offline checks do not install the driver
or restart a live session. Physical IPD adjustment, fpsVR refresh, wearer
acceptance and runtime frame-rate impact require a separate live validation.

This branch pairs with the native v21 client. The dashboard
adds **Steam Frame** and **Steam Frame Controllers** to headset and controller
emulation modes respectively. Select both in
Settings → Headset (and its Controllers section), then restart PC SteamVR as
requested by SURFXR and reconnect. Existing defaults are unchanged.

The PC driver uses SteamVR's built-in `frame_controller` input profile and
`frame_controller_left` / `frame_controller_right` models. SteamVR must contain
these Valve resources. The HMD reports Valve / Steam Frame / `frame_hmd`; SURFXR's
existing head tracking, optics, battery, gaze and encoder paths remain in use.

`frame_controller_inputs:1` in client capabilities is acknowledged only when
controllers use Steam Frame emulation. Frame then declares the Valve OpenXR
interaction profile with a complete 48-component set in each hand's profile
packet, since v21 replaces a global mapping manager on each packet.

| Hand | Independent click and touch | Scalar values |
| --- | --- | --- |
| Left | D-pad up/down/left/right, View, bumper, trigger, grip, thumbstick, system | trigger/grip, thumbstick X/Y |
| Right | A/B/X/Y, Menu, bumper, trigger, grip, thumbstick, system | trigger/grip, thumbstick X/Y |

The common input table and automatic mapping preserve physical hands and
component types, including releases. OpenVR uses `/input/grip`; the SURFXR wire
keeps `/input/squeeze`. `FrameInput.h` translates at registration/update only.
Menu/View have their own components and do not alias System. Other emulation
modes, original clients, and unacknowledged Frame clients keep the old behavior.

Frame's viewer offers **FULL CONTROLLER CAPTURE** in the settings menu. Manual
entry captures system click/touch through an optional headset SteamVR driver.
Four completed system clicks on one controller within two seconds exit capture.
Only system capture ends; ordinary forwarding and existing viewer shortcuts
continue. Capture is session-local, with focus/connection/standby cleanup and a
250 ms local lease. The PC does not initiate capture or handle the escape gesture.
See `../../docs/SURFXR.md` for installation/registration.

For compile-only validation, use
`cargo build -p surfxr_server_openvr -p surfxr_dashboard --release`.
Windows packaging uses the existing `cargo xtask build-streamer --release --keep-config`;
first close any Dashboard running from that output directory because the packager
replaces its files. This run compiled both programs, but packaging could not
replace the running Dashboard. The packager had already copied the new driver
DLL; `../../../build/cargo/release/surfxr_dashboard.exe` holds the corresponding new dashboard.
Set `LIBCLANG_PATH` to the existing libclang native directory if it is not already
configured (this workspace: `deps/libclang/clang/native`). Outputs are in
`../../../build/pc/surfxr_streamer_windows`; packaging does not register the driver in SteamVR.
Offline tests: `cargo test -p surfxr_common -p surfxr_server_core frame_ --lib`.

Validated: Windows Release server/dashboard build; all 48 paths are registered,
identity mapping retains press/release and hand, and negotiation requires both
ends. Frame's paired tests exercise native uploads and capture through isolated
loopback/fake OpenVR interfaces. Real Frame driver loading, system touch delivery,
headset rendering and wearer acceptance still require hardware verification.

The hardware layout follows [Valve's input documentation](https://partner.steamgames.com/doc/steamhardware/steamframe/input).


## Reliable controller forwarding and persistent offline display

The native v21 client negotiates `frame_controller_state:1` together with
`frame_controller_inputs:1`. Its initial value and toggle edges use the existing
reliable ReservedBuffer: fixed LE magic `0x56444353`, version:u32=1,
since_ns:u64 and enabled:u32 (20 bytes). The timestamp is the client's monotonic
clock, shared with tracking poll timestamps. Legacy clients keep motion-based
connection behavior. The desktop pose owner resets the prior VR session gate.

The OpenVR event owner applies state immediately to both virtual controllers,
releases game inputs/skeletons, submits disconnected poses and rejects controller
motion while disabled. Re-enable accepts only polls at/after the edge timestamp;
state messages must advance monotonically within a session. A negotiated new
session starts gated until the reliable initial state arrives. Disconnect also
clears the controller state without requiring another UDP packet.

SteamVR 2.17.10 independently advances activity timers for disconnected devices,
and vrmonitor prioritizes Standby over disconnected. `FrameControllerState.cpp`
uses the public, non-game `/proximity` input only while the Frame virtual device
is offline to suppress this timer. Activity is held at UserInteraction while
connection remains false; this is a compatibility guard, not a claim of physical
proximity or actual interaction. On valid reconnection the component becomes false.
ContainsProximitySensor remains false to retain ordinary online motion activity.
No game buttons or valid poses are synthesized; global SteamVR power settings,
runtime binaries, HMD proximity and other drivers are not modified.

Validation: `cargo test -p surfxr_server_core --lib` and
`cargo build -p surfxr_server_openvr --release`; paired Frame production Upload
checks cover initial/edge delivery without valid head tracking or a UDP socket,
and ensure unnegotiated/v20 sessions do not emit the extension. Real UI checks
must exceed the runtime's 300-second controller standby interval; connection
flags alone do not establish the displayed status. See the paired project's
`surfxr/docs/CONTROLLER_STATUS.md` for measured deployment results.
