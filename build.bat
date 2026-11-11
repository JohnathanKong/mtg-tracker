@echo off
setlocal
cd /d "%~dp0"
if not defined IDF_PATH (
  echo ERROR: ESP-IDF environment is not active.
  echo Open VS Code ^> Ctrl+Shift+P ^> ESP-IDF: Open ESP-IDF Terminal.
  exit /b 1
)
if not exist "sdkconfig" (
  echo Configuring ESP32-S3 target...
  call idf.py set-target esp32s3
  if errorlevel 1 exit /b 1
)
echo Building MTG Tracker ESP32-S3 firmware...
call idf.py build
exit /b %errorlevel%
