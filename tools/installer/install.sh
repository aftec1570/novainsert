#!/bin/bash
# Installs NovaInsert.vst3 for the current user (~/.vst3, no sudo). For all users copy it to /usr/lib/vst3/ instead.
set -e
mkdir -p ~/.vst3
rm -rf ~/.vst3/NovaInsert.vst3
cp -r "$(dirname "$0")/NovaInsert.vst3" ~/.vst3/
echo installed
