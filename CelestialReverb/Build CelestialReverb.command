#!/bin/bash
cd "$(dirname "$0")"
clear
START_TS=$(date +%s)
echo "═══════════════════════════════════════════"
echo "   PlugForge — Building CelestialReverb"
echo "═══════════════════════════════════════════"
echo ""
CONFIG="${PLUGFORGE_BUILD_CONFIG:-Release}"
FAST="${PLUGFORGE_FAST:-0}"
echo "Config: $CONFIG  |  Fast iterate: $FAST (set PLUGFORGE_FAST=1 to skip auval)"
echo ""
echo "Step 1 of 5 · Checking your Mac has the right tools..."
if ! xcode-select -p >/dev/null 2>&1; then
  echo "  ❌ Apple's developer tools are missing. A popup should appear — click Install, then run me again."
  xcode-select --install; read -p "Press Enter to close."; exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
  echo "  ❌ One tool (CMake) is missing. Open Terminal once, paste:  brew install cmake  — then run me again."
  read -p "Press Enter to close."; exit 1
fi
echo "  ✅ Tools ready."
echo ""
echo "Step 2 of 5 · Preparing the project (first time is slow — caches afterwards)..."
JUCE_FLAG=""
[ -d "$HOME/Projects/plugforge/vendor/JUCE-8.0.4" ] && JUCE_FLAG="-DFETCHCONTENT_SOURCE_DIR_JUCE=$HOME/Projects/plugforge/vendor/JUCE-8.0.4"
[ -d "$HOME/JUCE" ] && JUCE_FLAG="-DFETCHCONTENT_SOURCE_DIR_JUCE=$HOME/JUCE"
cmake -B build -G Xcode -DCMAKE_BUILD_TYPE=$CONFIG $JUCE_FLAG > .build_log.txt 2>&1 || { echo "  ❌ Preparation failed. Send .build_log.txt (in this folder) to Claude."; read -p "Press Enter to close."; exit 1; }
echo "  ✅ Project ready."
echo ""
echo "Step 3 of 5 · Compiling CelestialReverb ($CONFIG — incremental when possible)..."
cmake --build build --config $CONFIG --target CelestialReverb_AU CelestialReverb_VST3 >> .build_log.txt 2>&1 || { echo "  ❌ Compile failed. Send .build_log.txt to Claude."; read -p "Press Enter to close."; exit 1; }
cmake --build build --config $CONFIG --target CelestialReverb_Standalone >> .build_log.txt 2>&1 || true
cmake --build build --config $CONFIG >> .build_log.txt 2>&1 || true
echo "  ✅ Compiled."
echo ""
echo "Step 4 of 5 · Installing AU / VST3 / CLAP + refreshing Logic AU cache..."
export PLUGIN_NAME="CelestialReverb"
export BUILD_DIR="$(pwd)/build"
export CONFIG
export AU_SUBTYPE="bMuq"
export AU_MFR="Prll"
export AU_TYPE="aufx"
if [ "$FAST" = "1" ] || [ "$CONFIG" = "Debug" ]; then export SKIP_AUVAL=1; FAST=1; fi
bash "$(pwd)/scripts/macos-dev-install.sh" || { echo "  ❌ Install / AU refresh failed. See .dev-install.log"; read -p "Press Enter to close."; exit 1; }
echo "  ✅ Installed to ~/Library/Audio/Plug-Ins (system VST3 when writable)."
echo ""
echo "Step 5 of 5 · Logic compatibility..."
if [ "$FAST" = "1" ]; then
  echo "  ⚡ Fast mode — auval skipped. Run without PLUGFORGE_FAST=1 before shipping."
else
  if ! command -v auval >/dev/null 2>&1; then
    echo "  ❌ auval not found — mandatory for Release builds."
    read -p "Press Enter to close."; exit 1
  fi
  if auval -v aufx bMuq Prll >> .build_log.txt 2>&1; then
    echo "  ✅ Logic-compatible (auval PASS)."
  else
    echo "  ❌ auval FAILED. Send .build_log.txt to Claude."
    read -p "Press Enter to close."; exit 1
  fi
fi
END_TS=$(date +%s)
ELAPSED=$((END_TS-START_TS))
echo ""
echo "Iteration time: ${ELAPSED}s (code → installed AU)"
echo ""
echo "Audition tips (Logic can stay open):"
open "build/CelestialReverb_artefacts/$CONFIG/Standalone/CelestialReverb.app" 2>/dev/null || open "build/CelestialReverb_artefacts/Release/Standalone/CelestialReverb.app" 2>/dev/null || true
echo "  • Standalone opened for instant hear."
echo "  • In Logic: remove the plug-in from the slot and re-insert (loads new binary)."
echo "  • Hot path: PLUGFORGE_FAST=1 PLUGFORGE_BUILD_CONFIG=Debug ./DevIterate\ CelestialReverb.command"
echo ""
echo "Done."
read -p "Press Enter to close."
