@echo off
echo Building PhenixCode Dashboard...

echo Building SPA...
cd ..\spa-svelte
rmdir /s /q dist
rmdir /s /q node_modules
call npm install
call npm run build

echo Building webview...
cd ..\webview
rmdir /s /q build_rel
mkdir build_rel
cd build_rel
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
cd ..

echo Copying release artifacts to dist folder...
rmdir /s /q dist
mkdir dist
xcopy build_rel\Release\* dist\ /E /Y
del /f /q dist\output.log
del /f /q dist\diagnostics.log
rem xcopy assets\admconfig.json dist\
xcopy assets\* dist\ /E /Y

rem Exclude legacy settings only from the freshly created distribution staging folder.
del /f /q dist\settings.json dist\settings.default.json dist\phenixcode_projects\settings.json 2>nul

echo Build complete. Package is in dist folder!
