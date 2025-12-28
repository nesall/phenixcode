echo Building phenixcode-core release version...
rmdir /s /q build_rel
mkdir build_rel
cd build_rel
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release --parallel 
cd ..

set DIRNAME=dist

echo Copying release artifacts to %DIRNAME% folder...
rmdir /s /q %DIRNAME%
del /f /q %DIRNAME%.zip
mkdir %DIRNAME%
xcopy build_rel\Release\* %DIRNAME%\ /E /Y
xcopy build_rel\public %DIRNAME%\public\ /E /I
del /f /q %DIRNAME%\output.log 2>nul
del /f /q %DIRNAME%\diagnostics.log 2>nul
del /f /q %DIRNAME%\sqlite3_lib.lib 2>nul
xcopy assets\README %DIRNAME%\
xcopy assets\settings.template.json %DIRNAME%\
copy assets\settings.json %DIRNAME%\settings.json
copy assets\settings.json %DIRNAME%\settings.default.json
xcopy assets\bge_tokenizer.json %DIRNAME%\
rem xcopy scripts\install-service.bat %DIRNAME%\
rem xcopy scripts\uninstall-service.bat %DIRNAME%\
rem xcopy scripts\start.bat %DIRNAME%\
rem xcopy scripts\stop.bat %DIRNAME%\


echo Creating %DIRNAME%.zip...
powershell -NoProfile -Command "Compress-Archive -Path '%DIRNAME%\*' -DestinationPath '%DIRNAME%.zip' -Force"

echo Build complete. Package is in %DIRNAME% folder!
