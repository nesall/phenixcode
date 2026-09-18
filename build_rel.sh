#!/usr/bin/env bash
set -e

echo "Building phenixcode-core release version..." 
mkdir -p build_rel/out 
cd build_rel 
cmake -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=out -DCMAKE_BUILD_TYPE=Release .. 

echo "=== BUILDING ==="
cmake --build . --config Release --parallel 
cd .. 
echo "$(pwd)" 
echo "Copying release artifacts to dist/..." 
rm -rf dist 
rm -f dist.zip 
mkdir -p dist 
cp -r build_rel/out/* dist/

find . -name "phenixcode-core" -type f

echo "Final dist contents:";ls -la dist/

# Continue with other files
echo "Deleting .log files from dist/ if any..."
rm -f dist/output.log
rm -f dist/diagnostics.log
echo "Copying scripts and setup files to dist/ ..."
cp -r build_rel/public dist/ 2>/dev/null || echo "public folder not found, skipping"
cp assets/README dist/
#cp assets/settings.template.json dist/
cp assets/providers.json dist/providers.json
cp assets/settings.json dist/settings.json
cp assets/settings.json dist/settings.default.json
cp assets/bge_tokenizer.json dist/
mkdir -p dir/models
cp assets/baseline_v1.bin dist/models/
#cp scripts/install-service.sh dist/
#cp scripts/uninstall-service.sh dist/
#cp scripts/start.sh dist/
#cp scripts/stop.sh dist/

echo "Setting executable permissions..."
chmod +x dist/phenixcode-core

echo "Final permissions:"
ls -la dist/phenixcode-core

echo "Build complete. Package is in dist folder!"