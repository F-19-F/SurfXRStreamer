# SurfXR network endpoints

SurfXR uses its own private default port block, separate from ALVR and the earlier fork defaults. PC and Frame must be upgraded together.

| Purpose | Old default | SurfXR default | Transport / owner |
|---|---:|---:|---|
| Local OSC sender binding | 9942 | 19942 | UDP loopback; PC tracking |
| VR discovery/start/control | 9943 | 19943 | UDP discovery/start, TCP control; Dashboard/driver and Frame |
| VR media | 9944 | 19944 | Negotiated UDP or legacy TCP; PC/Frame |
| Split priority stream | 9945 | 19945 | Negotiated UDP/TCP; PC/Frame |
| Desktop admission/broker | 9946 | 19946 | TCP; Dashboard/daemon/Frame |
| Optional eye-gaze OSC receiver | 9945 | 19947 | UDP loopback; PC tracking |
| Desktop/unified control and discovery | 9950 | 19950 | TCP control, UDP discovery; daemon/Frame |
| Desktop/unified video | 9951 | 19951 | UDP; daemon/Frame |
| Desktop/unified audio/priority | 9952 | 19952 | UDP; daemon/Frame |
| Dashboard HTTP/WebSocket API | 8082 | 18082 | TCP; local by default |

The native contract lives in `shared/platform/include/surfxr/ports.hpp`; the Rust contract lives in `pc/surfxr-server/surfxr/common/src/ports.rs`. A cross-language test keeps them identical. Native default options, launcher configuration, discovery, daemon-to-driver loopback and shared desktop tickets all use this contract. The broker's selected port is still negotiated; explicit non-default custom ports remain supported.

## Existing settings and upgrades

PC session loading replaces only the old defaults for HTTP, media, local OSC and gaze OSC before creating network owners, then saves the migrated session. It preserves unrelated settings, trust, restart state, synchronization revisions and receipts. The native Frame connection normalizer replaces only old control 9943 and desktop 9950 values; the normalized configuration is used immediately and saved on the next configuration write. Profiles, hosts, remembered addresses and other custom port values remain intact. In service mode the updated daemon/legacy desktop executable also maps a persisted `--port 9950` argument to 19950 before binding; reinstalling/updating the service writes the new default into its command line. Explicit CLI/custom choices remain the operator's responsibility.

Install matching PC and Frame binaries and restart them through their existing lifecycle owners. An older running process keeps its old bindings until restarted. A source build does not install, restart services or rewrite installed user settings. Refresh manually port-restricted firewall rules to the new table. Windows Dashboard rules already allow the SurfXR executable paths; Linux firewalld/UFW/iptables templates cover the new stream/broker ports. Neither the templates nor their removal actions delete ALVR rules.

Third-party destination ports (for example VRChat/VRCFaceTracking) remain their public integration contracts; SurfXR's local sender binding is separate. Dynamic/ephemeral listeners continue to reserve available ports. Standard mDNS uses the shareable system discovery port 5353, with the independent `_surfxr._tcp.local.` service namespace; it is not a SurfXR-exclusive streaming listener.

Packet layouts, authentication, media framing, queue limits, trust and IPC acceptance serials are unchanged. Negotiated endpoint values and the mDNS service name are intentionally updated for coexistence. This is public network configuration, not runtime-internal adaptation. No new private runtime parsing, patches or version/address-dependent behavior is introduced.

## Verification boundary

Tests use CPU fixtures and isolated dynamically reserved loopback endpoints; no synthetic traffic is sent to a user's ALVR/SurfXR service. Regression coverage checks cross-language defaults, disjoint old/new listener sets, firewall profiles, persisted PC migration, Frame normalization/custom-port preservation, discovery and actual control/media loopback. Builds and offline tests do not certify simultaneous real ALVR/SurfXR streaming, headset installation or wearer acceptance.


Validated 2026-11-14: root PC Release build passed; selected Rust tests 135 passed / 1 pre-existing ignored, with a final common/server_io migration recheck (12 passed); Windows CTest 25/25; Linux x86_64 CTest 33/33; Python build/export/package/firewall-contract tests 46/46; shell syntax, source structure, documentation links and staged diff checks passed. The final Linux build report records three compiler warnings, including existing unused-parameter/vector diagnostics. Logs and input hashes are under repository `build/reports/port-*` and `build/frame/linux-x64/validation.json`.

The newly built daemon was invoked with `--service --dashboard-managed --port 9950 --self-test`: it reported migration to 19950 and completed its production offline module/protocol check without starting listeners/services, devices or capture. No installed user configuration or ALVR firewall rule was changed by validation. Runtime adaptation: **none added or changed**.
