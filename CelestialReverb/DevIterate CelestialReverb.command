#!/bin/bash
cd "$(dirname "$0")"
export PLUGFORGE_BUILD_CONFIG=Debug
export PLUGFORGE_FAST=1
exec bash "./Build CelestialReverb.command"
