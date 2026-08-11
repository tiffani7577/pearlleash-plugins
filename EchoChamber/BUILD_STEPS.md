# Build EchoChamber — Logic Pro iteration

## Fastest loop (developing)
1. Double-click **DevIterate EchoChamber.command** (Debug + skip auval + auto-install + AU cache refresh).
2. Audition in **Standalone**, or in Logic: remove the plug-in from the channel and re-insert (Logic can stay open).
3. Incremental rebuilds are typically **15–60s** on Apple Silicon after the first cold build.

## Release / validation loop
Double-click **Build EchoChamber.command**. Installs AU+VST3(+CLAP), refreshes the Audio Unit cache, runs `auval` (aufx / mbnK / Prll).

## Install locations
- AU: `~/Library/Audio/Plug-Ins/Components/EchoChamber.component`
- VST3: system `/Library/Audio/Plug-Ins/VST3` when writable, else user Library
- CLAP: `~/Library/Audio/Plug-Ins/CLAP/EchoChamber.clap` when built

## Logic without quitting
Often works after install + cache refresh if you **re-insert** the instance. Not true hot-reload — if the host keeps a locked mmap, remove the instance or quit Logic once. Plugin state survives when PLUGIN_CODE (`mbnK`) and manufacturer (`Prll`) stay stable (same product name).

## Manual install
`PLUGIN_NAME=EchoChamber BUILD_DIR=build CONFIG=Release bash scripts/macos-dev-install.sh`
