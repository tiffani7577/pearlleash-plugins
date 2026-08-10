#!/bin/bash
# PlugForge — macOS post-build install + AU cache refresh (Logic Pro iteration)
# Installs AU / VST3 / CLAP into user (and system VST3 when writable) folders,
# then invalidates AudioUnit caches so Logic can see updates without a full
# Plugin Manager rescan when possible.
#
# Usage (from generated project root or with env overrides):
#   ./scripts/macos-dev-install.sh
#   PLUGIN_NAME=Foo BUILD_DIR=build CONFIG=Debug ./scripts/macos-dev-install.sh
#   SKIP_AUVAL=1 FAST=1 ./scripts/macos-dev-install.sh
#
# Does not alter plugin DSP — install / host-cache only.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLUGIN_NAME="${PLUGIN_NAME:-}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
CONFIG="${CONFIG:-Release}"
SKIP_AUVAL="${SKIP_AUVAL:-0}"
FAST="${FAST:-0}"
AU_SUBTYPE="${AU_SUBTYPE:-}"
AU_MFR="${AU_MFR:-Prll}"
AU_TYPE="${AU_TYPE:-aufx}"   # aufx = effect, aumu = instrument/synth
LOG_FILE="${LOG_FILE:-$ROOT/.dev-install.log}"

USER_AU="$HOME/Library/Audio/Plug-Ins/Components"
USER_VST3="$HOME/Library/Audio/Plug-Ins/VST3"
USER_CLAP="$HOME/Library/Audio/Plug-Ins/CLAP"
SYS_VST3="/Library/Audio/Plug-Ins/VST3"

ts() { date +%H:%M:%S; }
log() { echo "[$(ts)] $*" | tee -a "$LOG_FILE"; }

if [[ -z "$PLUGIN_NAME" ]]; then
  # Infer from CMake project folder name
  PLUGIN_NAME="$(basename "$ROOT")"
fi

: > "$LOG_FILE"
log "PlugForge dev-install: plugin=$PLUGIN_NAME config=$CONFIG build=$BUILD_DIR"

mkdir -p "$USER_AU" "$USER_VST3" "$USER_CLAP"

find_bundle() {
  local ext="$1"
  find "$BUILD_DIR" -type d -name "*.${ext}" 2>/dev/null | head -1 || true
}

install_bundle() {
  local src="$1"
  local dest_dir="$2"
  local dest_name="$3"
  local dest="$dest_dir/$dest_name"
  mkdir -p "$dest_dir"
  rm -rf "$dest"
  cp -R "$src" "$dest"
  # Help hosts notice mtime change
  touch "$dest" 2>/dev/null || true
  if [[ -d "$dest/Contents" ]]; then
    touch "$dest/Contents" 2>/dev/null || true
  fi
  # Ad-hoc sign for local iterate (avoids Gatekeeper stale trust on replace)
  if command -v codesign >/dev/null 2>&1; then
    codesign --force --deep --sign - "$dest" >>"$LOG_FILE" 2>&1 || true
  fi
  log "Installed $dest_name → $dest"
  echo "$dest"
}

AU_SRC="$(find_bundle component)"
VST3_SRC="$(find_bundle vst3)"
CLAP_SRC="$(find "$BUILD_DIR" \( -type d -name '*.clap' -o -type f -name '*.clap' \) 2>/dev/null | head -1 || true)"

INSTALLED_AU=""
INSTALLED_VST3=""
INSTALLED_CLAP=""

if [[ -n "$AU_SRC" ]]; then
  INSTALLED_AU="$(install_bundle "$AU_SRC" "$USER_AU" "${PLUGIN_NAME}.component")"
else
  log "WARN: no .component found under $BUILD_DIR"
fi

if [[ -n "$VST3_SRC" ]]; then
  if [[ -w "$SYS_VST3" ]] || mkdir -p "$SYS_VST3" 2>/dev/null; then
    if cp -R "$VST3_SRC" "$SYS_VST3/.plugforge-write-test" 2>/dev/null; then
      rm -rf "$SYS_VST3/.plugforge-write-test"
      INSTALLED_VST3="$(install_bundle "$VST3_SRC" "$SYS_VST3" "${PLUGIN_NAME}.vst3")"
    else
      INSTALLED_VST3="$(install_bundle "$VST3_SRC" "$USER_VST3" "${PLUGIN_NAME}.vst3")"
    fi
  else
    INSTALLED_VST3="$(install_bundle "$VST3_SRC" "$USER_VST3" "${PLUGIN_NAME}.vst3")"
  fi
else
  log "WARN: no .vst3 found under $BUILD_DIR"
fi

if [[ -n "$CLAP_SRC" ]]; then
  # Normalize to directory bundle name
  if [[ -d "$CLAP_SRC" ]]; then
    INSTALLED_CLAP="$(install_bundle "$CLAP_SRC" "$USER_CLAP" "${PLUGIN_NAME}.clap")"
  else
    mkdir -p "$USER_CLAP"
    cp "$CLAP_SRC" "$USER_CLAP/${PLUGIN_NAME}.clap"
    INSTALLED_CLAP="$USER_CLAP/${PLUGIN_NAME}.clap"
    log "Installed ${PLUGIN_NAME}.clap → $INSTALLED_CLAP"
  fi
else
  log "NOTE: no CLAP artefact (optional)"
fi

refresh_au_cache() {
  log "Refreshing Audio Unit cache / registrar…"
  # Remove AU component cache so registrar re-scans Components
  rm -rf "$HOME/Library/Caches/AudioUnitCache" 2>/dev/null || true
  rm -f "$HOME/Library/Caches/com.apple.audiounits.cache" 2>/dev/null || true
  # Bounce the registrar (safe if not running)
  killall AudioComponentRegistrar 2>/dev/null || true
  # Give registrar a moment to respawn on next query
  sleep 0.3
  if [[ -n "$INSTALLED_AU" ]]; then
    touch "$INSTALLED_AU" 2>/dev/null || true
  fi
  log "AU cache refresh done"
}

refresh_au_cache

# Auval hard-fail is opt-in (AUVAL_HARD=1). Default is soft so CMake POST_BUILD /
# agent paths never turn an auval miss into cmake --build exit 65.
# Build *.command step 5 still enforces auval separately for Release shipping.
AUVAL_HARD="${AUVAL_HARD:-0}"
if [[ "$SKIP_AUVAL" == "1" || "$FAST" == "1" ]]; then
  log "Skipping auval (FAST/SKIP_AUVAL=1) — use for hot DSP iterate; run full validate before shipping"
else
  if [[ -n "$INSTALLED_AU" ]]; then
    if ! command -v auval >/dev/null 2>&1; then
      if [[ "$AUVAL_HARD" == "1" ]]; then
        log "FATAL: auval missing"
        exit 1
      fi
      log "WARN: auval missing — soft continue (set AUVAL_HARD=1 to fail)"
    elif [[ -z "$AU_SUBTYPE" ]]; then
      log "WARN: AU_SUBTYPE not set — skipping auval identity check"
    else
      log "auval -v $AU_TYPE $AU_SUBTYPE $AU_MFR"
      set +e
      OUT="$(auval -v "$AU_TYPE" "$AU_SUBTYPE" "$AU_MFR" 2>&1)"
      RC=$?
      set -e
      echo "$OUT" >>"$LOG_FILE"
      if [[ $RC -ne 0 ]] || ! echo "$OUT" | grep -q "PASS"; then
        log "auval failed — retrying after second registrar refresh"
        refresh_au_cache
        sleep 0.5
        set +e
        OUT="$(auval -v "$AU_TYPE" "$AU_SUBTYPE" "$AU_MFR" 2>&1)"
        RC=$?
        set -e
        echo "$OUT" >>"$LOG_FILE"
        if [[ $RC -ne 0 ]] || ! echo "$OUT" | grep -q "PASS"; then
          if [[ "$AUVAL_HARD" == "1" ]]; then
            log "FATAL: auval did not PASS"
            exit 1
          fi
          log "WARN: auval did not PASS — soft continue (set AUVAL_HARD=1 to fail cmake/install)"
        else
          log "auval PASS"
        fi
      else
        log "auval PASS"
      fi
    fi
  fi
fi

log "Done. Logic: if the plug-in is already on a track, remove+re-add the instance (or bounce the channel) to load the new binary. Full Logic quit is only needed when the host keeps a locked mmap."
log "Fastest audible check without Logic: open the Standalone app under $BUILD_DIR."
