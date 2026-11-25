@echo off
rem SPDX-License-Identifier: BSD-2-Clause-Patent
rem
rem Windows Setup launcher for a T70 Pro install stick made by make-usb.
rem winpeshl.ini starts it in the Setup WinPE (boot.wim index 2) of the
rem official Windows ISO; everything it uses is in X:\T70, put there by
rem make-usb. It loads the T70 drivers (the stick itself sits behind the
rem T70 USB controller), runs the stick's own sources\setup.exe, and before
rem the first restart gives the new Windows the same drivers and test
rem signing, which its first boot needs to reach its own disk.
setlocal EnableExtensions DisableDelayedExpansion
set "T70=%SystemDrive%\T70"
set "LOG=%T70%\StartT70Setup.log"
set "installMedia="
call :Log "T70 Pro Windows Setup launcher"

wpeinit.exe
if errorlevel 1 (
    call :Fail "WinPE initialization failed."
    exit /b 1
)

rem The drivers are test signed; trust their certificates before loading them.
reg.exe import "%T70%\trust-pe.reg" >>"%LOG%" 2>&1
if errorlevel 1 (
    call :Fail "Could not add the driver certificates."
    exit /b 1
)
for /f "usebackq eol=; delims=" %%I in ("%T70%\Drivers\order.txt") do call :Load "%%I"

rem The tablet has no TPM and its SoC is not on the Windows 11 CPU list.
for %%C in (TPM SecureBoot CPU RAM Storage) do reg.exe add "HKLM\SYSTEM\Setup\LabConfig" /v Bypass%%CCheck /t REG_DWORD /d 1 /f >>"%LOG%" 2>&1

rem Search for the installation files while device discovery continues.
for /l %%R in (1,1,60) do (
    set "installMedia="
    set "ambiguous=0"
    for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do call :Probe %%D:
    call :Resolve
    if errorlevel 3 goto :Found
    if errorlevel 2 exit /b 2
    if errorlevel 1 exit /b 1
    ping.exe -n 2 127.0.0.1 >nul 2>&1
)
call :Fail "No Windows installation media was found (USB driver not started?)."
exit /b 1

:Found
call :Log "Installation media %installMedia%"
rem Keep the boot store an existing Windows may already have, to tell later
rem whether Setup installed anything.
call :MountEsp
if errorlevel 2 exit /b 1
if defined espFound copy /y "%ESP%\EFI\Microsoft\Boot\BCD" "%T70%\bcd-before" >nul 2>&1
if defined espMounted mountvol.exe %ESP% /D >nul 2>&1
rem The media root setup.exe is the in-OS upgrade path (0x80070103-0x40031 in WinPE).
start "" /wait "%installMedia%\sources\setup.exe" /noreboot
call :Log "setup.exe returned %errorlevel%"

call :MountEsp
if errorlevel 2 exit /b 1
if not defined espFound goto :NotInstalled
if exist "%T70%\bcd-before" fc.exe /b "%T70%\bcd-before" "%ESP%\EFI\Microsoft\Boot\BCD" >nul 2>&1
if exist "%T70%\bcd-before" if not errorlevel 1 (
    if defined espMounted mountvol.exe %ESP% /D >nul 2>&1
    goto :NotInstalled
)
call :FindTarget
if errorlevel 1 exit /b 1
call :Log "New Windows on %target%, boot files on %ESP%"
bcdedit.exe /store "%ESP%\EFI\Microsoft\Boot\BCD" /set {default} testsigning on >>"%LOG%" 2>&1
if errorlevel 1 (
    call :Fail "Could not switch on test signing for the new Windows."
    exit /b 1
)
dism.exe /Image:%target%\ /Add-Driver /Driver:"%T70%\Drivers" /Recurse /English /LogPath:"%T70%\dism-add-driver.log" >>"%LOG%" 2>&1
if errorlevel 1 (
    call :Fail "Could not add the T70 drivers to the new Windows."
    exit /b 1
)
reg.exe load HKLM\T70OS "%target%\Windows\System32\config\SOFTWARE" >>"%LOG%" 2>&1
if errorlevel 1 (
    call :Fail "Could not open the new Windows registry."
    exit /b 1
)
reg.exe import "%T70%\trust-os.reg" >>"%LOG%" 2>&1
set "trustResult=%errorlevel%"
reg.exe unload HKLM\T70OS >>"%LOG%" 2>&1
if not "%trustResult%"=="0" (
    call :Fail "Could not add the driver certificates to the new Windows."
    exit /b 1
)
if defined espMounted mountvol.exe %ESP% /D >>"%LOG%" 2>&1
call :Log "Done; restarting into the new Windows."
call :SaveLog
wpeutil.exe reboot
exit /b 0

:NotInstalled
call :Log "Setup ended without a new Windows installation; nothing changed."
call :SaveLog
exit /b 0

:Load
drvload.exe "%T70%\Drivers\%~1" >>"%LOG%" 2>&1
if errorlevel 1 (call :Log "drvload failed: %~1") else call :Log "loaded %~1"
exit /b 0

:Probe
if /i "%~1"=="%SystemDrive%" exit /b 0
if not exist "%~1\sources\setup.exe" exit /b 0
set "hasImage="
if exist "%~1\sources\install.wim" set "hasImage=1"
if exist "%~1\sources\install.esd" set "hasImage=1"
for %%I in ("%~1\sources\install*.swm") do if exist "%%~I" set "hasImage=1"
if not defined hasImage exit /b 0
if not defined installMedia set "installMedia=%~1"
if defined installMedia if /i not "%~1"=="%installMedia%" set "ambiguous=1"
exit /b 0

:Resolve
if "%ambiguous%"=="1" (
    call :Fail "More than one Windows installation medium was found."
    exit /b 2
)
if not defined installMedia exit /b 0
exit /b 3

:PickLetter
rem A drive letter no volume uses, from mountvol's own list.
for %%L in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do set "used_%%L="
for /f "tokens=1" %%V in ('mountvol.exe') do call :NoteLetter "%%V"
set "ESP="
for %%L in (S T U V W Y Z R Q P O N M L K) do if not defined ESP if not defined used_%%L set "ESP=%%L:"
if not defined ESP (
    call :Fail "No free drive letter."
    exit /b 1
)
exit /b 0

:NoteLetter
set "entry=%~1"
if "%entry:~1,2%"==":\" if "%entry:~3%"=="" set "used_%entry:~0,1%=1"
exit /b 0

:MountEsp
rem The EFI system partition, where Setup puts the new Windows' boot files.
rem Right after Setup it still has the letter Setup gave it; otherwise it
rem takes one only through mountvol /S.
set "espFound="
set "espMounted="
set "espCount=0"
set "ESP="
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do call :EspLetter %%D:
if "%espCount%"=="1" (
    set "espFound=1"
    call :Log "Windows boot files already on %ESP%"
    exit /b 0
)
if not "%espCount%"=="0" (
    call :Fail "Found %espCount% drives with Windows boot files; expected one."
    exit /b 2
)
call :PickLetter
if errorlevel 1 exit /b 2
mountvol.exe %ESP% /S >>"%LOG%" 2>&1
if errorlevel 1 (
    call :Log "No EFI system partition to mount."
    exit /b 0
)
if exist "%ESP%\EFI\Microsoft\Boot\BCD" (
    set "espFound=1"
    set "espMounted=1"
) else mountvol.exe %ESP% /D >nul 2>&1
call :Log "EFI system partition mounted on %ESP%, Windows boot files: %espFound%"
exit /b 0

:EspLetter
if /i "%~1"=="%SystemDrive%" exit /b 0
if /i "%~1"=="%installMedia%" exit /b 0
if not exist "%~1\EFI\Microsoft\Boot\BCD" exit /b 0
if exist "%~1\sources\setup.exe" exit /b 0
set "ESP=%~1"
set /a espCount+=1
exit /b 0

:FindTarget
set "osdevice="
for /f "tokens=1,2" %%A in ('bcdedit.exe /store "%ESP%\EFI\Microsoft\Boot\BCD" /enum {default}') do if /i "%%A"=="osdevice" set "osdevice=%%B"
if /i not "%osdevice:~0,10%"=="partition=" (
    call :Fail "The new boot entry does not name a Windows partition (%osdevice%)."
    exit /b 1
)
set "target=%osdevice:~10%"
if not exist "%target%\Windows\System32\config\SOFTWARE" (
    call :Fail "No Windows found on %target%."
    exit /b 1
)
exit /b 0

:Log
>>"%LOG%" echo [%date% %time%] %~1
echo %~1
exit /b 0

:SaveLog
if not defined installMedia exit /b 0
copy /y "%LOG%" "%installMedia%\T70-setup.log" >nul 2>&1
if exist "%T70%\dism-add-driver.log" copy /y "%T70%\dism-add-driver.log" "%installMedia%\T70-dism.log" >nul 2>&1
exit /b 0

:Fail
call :Log "FAIL: %~1"
call :SaveLog
echo.
echo Installation stopped: %~1
echo Log: X:\T70\StartT70Setup.log (also T70-setup.log on the stick if it was found)
pause
exit /b 1
