@echo off
rem SPDX-License-Identifier: BSD-2-Clause-Patent
rem Windows entry point of the T70 Pro stick maker; see make-usb.ps1.
rem Run it from an administrator terminal.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-usb.ps1" %*
exit /b %errorlevel%
