#!/usr/bin/env bash
set -euo pipefail

# Resolve all build and package paths relative to this script.
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

# --- Parse version.cmake ---
MAJOR=$(grep '^set(EMBEDDER_VERSION_MAJOR' version.cmake | sed -E 's/.* ([0-9]+).*/\1/')
MINOR=$(grep '^set(EMBEDDER_VERSION_MINOR' version.cmake | sed -E 's/.* ([0-9]+).*/\1/')
PATCH=$(grep '^set(EMBEDDER_VERSION_PATCH' version.cmake | sed -E 's/.* ([0-9]+).*/\1/')

VER="${MAJOR}.${MINOR}.${PATCH}"
NAME="phenixcode-v${VER}-linux-x64"

echo "VER=$VER"
echo "NAME=$NAME"

EMBEDDER="./"
CLIENT="ui/clients/webview"
DASHBOARD="ui/dashboard/webview"

(cd "$EMBEDDER" && ./build_rel.sh)
echo "FINISHED $EMBEDDER"

(cd "$CLIENT" && ./build_rel.sh)

(cd "$DASHBOARD" && ./build_rel.sh)

rm -rf "$NAME"
mkdir "$NAME"

cp -R "$EMBEDDER/dist/." "$NAME/"
cp -R "$CLIENT/dist/." "$NAME/"
cp -R "$DASHBOARD/dist/." "$NAME/"

echo "$NAME.zip..."
rm -f "$NAME.zip"
zip -r "$NAME.zip" "$NAME" >/dev/null

rm -rf "$NAME"

echo "Package '$NAME.zip' ready."
