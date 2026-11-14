# Build and launch entry points

For local WSL builds, follow [WSL_BUILD.md](WSL_BUILD.md) and invoke `build_viewer.py --local`. This path never opens SSH or deploys.

For an explicitly selected independent ARM64 SSH build host, from the repository root:

```powershell
.\Build-PC.ps1 -PrepareDependencies
.\Start-Frame.ps1 -Plan -BuildOnly -FrameSsh builder@linux-builder
.\Start-Frame.ps1 -BuildOnly -FrameSsh builder@linux-builder
```

`scripts/Start-Frame.cmd` and `scripts/Start-FrameSingle.cmd` forward to the corresponding PowerShell entries. The default link is direct 6 GHz. Use `-Link lan` and explicit `-PcLan` / `-FrameLan` when needed. Set `-PcDirect` / `-FrameDirect` for explicit direct-link addresses. Empty PC addresses select discovery.

`-BuildOnly` stops after compile and offline CTest. The source entry uses the same SSH target for building and deployment; it is not a split build-host/headset pipeline. Do not remove `-BuildOnly` on an independent builder to deploy to a different device. Prefer local WSL builds and precompiled release installation on the headset; compiling on a headset requires an explicit request.

Without `-BuildOnly`, the source entry can stop the managed viewer, deploy, configure services and request the selected mode. `-NoStart` only leaves streaming idle after deployment; it is not a build-only switch. PC SurfXR Dashboard is independently started by the user and owns device trust. Building the PC software does not install or restart its services or automatically run the separate native/Cargo tests; see [TESTING.md](TESTING.md).

`scripts/Install-Frame.ps1 -ReleaseDirectory <unpacked-release>` installs a precompiled package. `-Plan` verifies the local package and prints actual SSH/SCP commands, remote shell content and JSON stdin without contacting Frame; specify the package directory even for a preview. The remote staging directory remains an explicit variable until preparation returns its value. `-Action uninstall` preserves settings; `-Action purge` removes the installed client and its saved data. Do not run purge as routine maintenance.

`builder@linux-builder` and the installation default `steamos@frame` are placeholders configured by the operator, not publicly available hosts. The selected host must already be trusted by SSH. Build reports record source and binary hashes; packaging refuses changed sources. For an overview of command side effects, see [GETTING_STARTED.md](GETTING_STARTED.md).


PC Dashboard **Installation / Steam Frame client** uses the same release entry with `-PairOnInstall`. It reads the installed target's viewer-compatible hostname and SSH server IP through authenticated SSH, then lets the current Dashboard session owner save trust. The UI waits for a trusted session snapshot before confirming pairing. This private adapter option requires release install; Plan never contacts a device, and BuildOnly/removal cannot pair. Manual script installs retain explicit pairing in Devices. Captured plans and results stay visible in Dashboard; SSH owns interactive host-key/password prompts. If a Dashboard target cannot resolve, enter its IPv4 address in the SSH field and retry.

Update PC `frame-installer/` with Dashboard: it contains the matching PC-side adapter, independently of the chosen ARM64 package in `frame-client/`. Updating only the executable can otherwise leave new Dashboard parameters incompatible with an old archive's installer. Preview includes the pairing identity command and uses the same command generator as execution. Entry-point errors, including missing launch scripts, are returned to the persistent result area instead of only an exit code.
