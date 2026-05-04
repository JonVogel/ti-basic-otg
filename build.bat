@echo off
REM Compile and upload TI BASIC sketch to ESP32-S3-USB-OTG
REM
REM Usage:
REM   build.bat                   - compile + upload (default port)
REM   build.bat compile           - compile only
REM   build.bat upload            - upload only
REM   build.bat monitor           - open serial monitor
REM   build.bat all               - compile + upload + monitor
REM
REM Each form accepts an optional COM port:
REM   build.bat upload  COM23
REM   build.bat all     COM7
REM   build.bat         COM5     (compile+upload on COM5)
REM   build.bat monitor COM4

setlocal enabledelayedexpansion

set "DEFAULT_PORT=COM5"
set "ACTION=%~1"
set "PORT_ARG=%~2"

if "!PORT_ARG!"=="" (
  set "FIRST=!ACTION!"
  if /i "!FIRST:~0,3!"=="COM" (
    set "PORT_ARG=!ACTION!"
    set "ACTION="
  )
)

if "!PORT_ARG!"=="" (
  set "PORT=!DEFAULT_PORT!"
) else (
  set "PORT=!PORT_ARG!"
)

set "FQBN=esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=8M"
set "SKETCH_DIR=%~dp0"
if "!SKETCH_DIR:~-1!"=="\" set "SKETCH_DIR=!SKETCH_DIR:~0,-1!"

echo.
echo === build.bat (ti-basic-otg): action='!ACTION!' port='!PORT!' ===

if "!ACTION!"==""           goto compile_upload
if /i "!ACTION!"=="compile" goto compile
if /i "!ACTION!"=="upload"  goto upload
if /i "!ACTION!"=="monitor" goto monitor
if /i "!ACTION!"=="all"     goto all
goto compile_upload

:compile
echo.
echo === Compiling ===
arduino-cli compile --fqbn !FQBN! --libraries "!SKETCH_DIR!" "!SKETCH_DIR!"
goto end

:upload
echo.
echo === Uploading to !PORT! ===
arduino-cli upload -p !PORT! --fqbn !FQBN! "!SKETCH_DIR!"
goto end

:compile_upload
echo.
echo === Compiling ===
arduino-cli compile --fqbn !FQBN! --libraries "!SKETCH_DIR!" "!SKETCH_DIR!"
if errorlevel 1 goto end
echo.
echo === Uploading to !PORT! ===
arduino-cli upload -p !PORT! --fqbn !FQBN! "!SKETCH_DIR!"
goto end

:monitor
echo.
echo === Monitoring !PORT! (Ctrl+C to exit) ===
arduino-cli monitor -p !PORT! --config baudrate=115200
goto end

:all
echo.
echo === Compiling ===
arduino-cli compile --fqbn !FQBN! --libraries "!SKETCH_DIR!" "!SKETCH_DIR!"
if errorlevel 1 goto end
echo.
echo === Uploading to !PORT! ===
arduino-cli upload -p !PORT! --fqbn !FQBN! "!SKETCH_DIR!"
if errorlevel 1 goto end
echo.
echo === Monitoring !PORT! (Ctrl+C to exit) ===
arduino-cli monitor -p !PORT! --config baudrate=115200
goto end

:end
endlocal
