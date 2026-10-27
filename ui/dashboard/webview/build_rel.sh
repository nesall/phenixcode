#!/bin/bash
set -euo pipefail
echo "Building PhenixCode Dashboard..."

# Build the SPA first
echo "Building SPA..."
cd ../spa-svelte
rm -rf dist
rm -rf node_modules
npm install
npm run build

# Build the webview
echo "Building webview..."
cd ../webview
rm -rf build_rel
mkdir -p build_rel/out
cd build_rel
cmake -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=out -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release --parallel
cd ..

echo "Copying release artifacts to dist folder..."
rm -rf dist
mkdir -p dist
cp -R build_rel/out/. dist/
rm -f dist/output.log
rm -f dist/diagnostics.log
#cp assets/admconfig.json dist/
cp -R assets/. dist/

echo "Setting executable permissions..."
chmod +x dist/phenixcode-admin

echo "Final permissions:"
ls -la dist/phenixcode-admin

echo "Build complete. Package is in dist folder!"

