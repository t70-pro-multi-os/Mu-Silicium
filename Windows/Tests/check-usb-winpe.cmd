@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Run from WinPE after booting with the driver packages in its standard store.
wpeinit.exe

set "usb="
for /l %%R in (1,1,20) do (
    for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do call :CheckUSB %%D:
    if defined usb goto pass
    ping.exe -n 2 127.0.0.1 >nul 2>&1
)

echo FAIL: WinPE could not read the USB fixture.
pnputil.exe /enum-devices /instanceid "ACPI\HANA6789\6"
pnputil.exe /enum-devices /instanceid "ACPI\HANA678D\0"
exit /b 1

:CheckUSB
if /i "%~1"=="%SystemDrive%" exit /b 0
if not exist "%~1\T70USB.TAG" exit /b 0
set "contents="
set /p "contents="<"%~1\T70USB.TAG"
if "%contents%"=="T70-WINPE-USB-READ-V1" set "usb=%~1"
exit /b 0

:pass
echo PASS: WinPE read the expected bytes from %usb%\T70USB.TAG.
exit /b 0
