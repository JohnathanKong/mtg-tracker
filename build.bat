@echo off
setlocal
cd /d "%~dp0"

if not defined IDF_PATH (
  echo ERROR: ESP-IDF environment is not active.
  echo Open VS Code ^> Ctrl+Shift+P ^> ESP-IDF: Open ESP-IDF Terminal.
  exit /b 1
)

REM First build with the LCD: regenerate the existing ESP-IDF config
REM only when the older receiver build did not have PSRAM enabled.
REM Save its settings safely; do not remove source code or saved decks.
if exist "sdkconfig" (
  findstr /B /C:"CONFIG_SPIRAM=y" "sdkconfig" >nul
  if errorlevel 1 (
    echo Previous configuration has no display PSRAM; migrating once.
    if not exist "sdkconfig.pre-display-backup" (
      copy /Y "sdkconfig" "sdkconfig.pre-display-backup" >nul
      if errorlevel 1 exit /b 1
    )
    del /Q "sdkconfig"
    if errorlevel 1 exit /b 1
  )
)

if not exist "sdkconfig" (
  echo Configuring ESP32-S3 target with LCD defaults...
  call idf.py set-target esp32s3
  if errorlevel 1 exit /b 1
)

echo Building MTG Tracker ESP32-S3 LCD firmware...
call idf.py build
exit /b %errorlevel%
