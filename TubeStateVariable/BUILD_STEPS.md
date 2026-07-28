# Build TubeStateVariable — no terminal needed

## The normal way
Double-click **Build TubeStateVariable.command** in this folder. It checks your tools, compiles, installs, runs Apple's Logic-compatibility check, and opens the plugin as an app when it finishes — all in plain English. Rebuild after any design change by double-clicking it again.

First time only, macOS may ask permission to run it: right-click → Open → Open.

## Where your plugin ends up
- Logic / any AU host: Audio Units → PearlLeashPlugin → TubeStateVariable (fully quit and reopen Logic first)
- VST3 hosts: ~/Library/Audio/Plug-Ins/VST3/TubeStateVariable.vst3
- Try-it-now app: opens automatically after a successful build

## If Logic doesn't show it
1. Fully quit Logic (Cmd+Q) and reopen — it scans plugins at launch.
2. Still missing? Double-click the build file again and screenshot what it says.

## If anything fails
The build file writes everything technical to .build_log.txt in this folder. Send that file to Claude — you never need to read it.

## Terminal fallback (only if the double-click path is blocked)
    cmake -B build -G Xcode
    cmake --build build --config Release
