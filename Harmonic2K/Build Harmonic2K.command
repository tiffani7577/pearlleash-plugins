#!/bin/bash
cd "$(dirname "$0")"
clear
echo "═══════════════════════════════════════════"
echo "   PlugForge — Building Harmonic2K"
echo "═══════════════════════════════════════════"
echo ""
echo "Step 1 of 4 · Checking your Mac has the right tools..."
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
echo "Step 2 of 4 · Preparing the project (first time is slow — it downloads the audio framework)..."
JUCE_FLAG=""
[ -d "$HOME/Projects/plugforge/vendor/JUCE-8.0.4" ] && JUCE_FLAG="-DFETCHCONTENT_SOURCE_DIR_JUCE=$HOME/Projects/plugforge/vendor/JUCE-8.0.4"
cmake -B build -G Xcode $JUCE_FLAG > .build_log.txt 2>&1 || { echo "  ❌ Preparation failed. Send .build_log.txt (in this folder) to Claude."; read -p "Press Enter to close."; exit 1; }
echo "  ✅ Project ready."
echo ""
echo "Step 3 of 4 · Compiling your plugin (a few minutes the first time)..."
cmake --build build --config Release >> .build_log.txt 2>&1 || { echo "  ❌ Compile failed. Send .build_log.txt (in this folder) to Claude."; read -p "Press Enter to close."; exit 1; }
echo "  ✅ Compiled. It was auto-installed into your plugin folders."
echo ""
echo "Step 4 of 4 · Checking Logic compatibility (Apple's official validation)..."
if auval -v aufx PHar Prll >> .build_log.txt 2>&1; then
  echo "  ✅ Logic-compatible. Fully quit and reopen Logic — find it under Audio Units → PearlLeashPlugin."
else
  echo "  ⚠️  Compatibility check flagged something. Plugin may still work — send .build_log.txt to Claude."
fi
echo ""
echo "Opening the standalone app so you can try it right now..."
open "build/Harmonic2K_artefacts/Release/Standalone/Harmonic2K.app" 2>/dev/null
echo ""
echo "Done. Rebuilding after design changes: just double-click me again."
read -p "Press Enter to close."
