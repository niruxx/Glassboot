@echo off
rem Glassboot Windows PE helper.
rem
rem wimboot injects this file into X:\Windows\System32 of the ISO's boot.wim, together
rem with a winpeshl.ini that runs it. Windows PE cannot see the ISO GRUB booted from, so
rem this script finds the GLASSBOOT data partition, reads the choices GRUB saved in
rem \glassboot\grubenv, and attaches the ISO as a virtual DVD (glassboot-attach.exe).
rem
rem   glassboot.cmd               Windows installers (winpeshl.ini): wpeinit, attach the
rem                               ISO, apply options, start Windows Setup.
rem   glassboot.cmd mountonly     Windows PE tool discs (winpeshl-tool.ini, in the
rem                               background): attach the ISO and read the disc's own
rem                               winpeshl.ini while the disc's startup (wpeinit) runs.
rem   glassboot.cmd toolfinish    Tool discs, after wpeinit: run the rest of the disc's
rem                               own startup.
setlocal EnableExtensions EnableDelayedExpansion
set "LETTERS=C D E F G H I J K L M N O P Q R S T U V W Y Z"
set "STATE=%SystemRoot%\TEMP"
if /i "%~1"=="mountonly" goto :mountonly
if /i "%~1"=="toolfinish" goto :toolfinish

rem ===========================================================================
rem Windows installers
rem ===========================================================================
title Glassboot
wpeinit
call :find_data 30
if errorlevel 1 goto :shell
call :read_options
call :pick_iso interactive
if errorlevel 1 goto :shell
call :load_drivers
call :attach
if errorlevel 1 goto :shell

rem ---- Windows 11 requirement checks (read by Windows Setup from the WinPE registry)
if "!OPT_BYPASS!"=="1" (
  echo Glassboot: skipping Windows 11 hardware checks
  for %%k in (BypassTPMCheck BypassSecureBootCheck BypassRAMCheck BypassCPUCheck BypassStorageCheck) do (
    reg add HKLM\SYSTEM\Setup\LabConfig /v %%k /t REG_DWORD /d 1 /f >nul
  )
)

rem ---- answer file: a user unattend.xml from the drive, or our local-account one
set "ARGS="
if defined OPT_UNATTEND (
  set "OPT_UNATTEND=!OPT_UNATTEND:/=\!"
  set "UNATTEND=!DATA!!OPT_UNATTEND!"
  if exist "!UNATTEND!" (
    set "ARGS=/unattend:"!UNATTEND!""
  ) else (
    echo Answer file !UNATTEND! not found; continuing without it.
  )
) else if "!OPT_LOCAL!"=="1" (
  set "ARGS=/unattend:"%SystemRoot%\System32\glassboot-localacct.xml""
)

call :user_hook
echo Glassboot: starting Windows Setup from !SRC! !ARGS!
"!SRC!\sources\setup.exe" !ARGS!
wpeutil reboot
exit /b 0

rem ===========================================================================
rem Windows PE tool discs, step 1 (background, while the disc's wpeinit runs)
rem ===========================================================================
:mountonly
title Glassboot: mounting the ISO
call :find_data 90
if errorlevel 1 goto :mounted
call :read_options
call :pick_iso
if errorlevel 1 goto :mounted
call :load_drivers
call :attach
if errorlevel 1 goto :mounted
rem Our winpeshl.ini replaced the disc's: read the original from the ISO's boot.wim
rem (mounted read-only with DISM) so :toolfinish can run its remaining entries.
set "MNT=X:\glassboot-wim"
mkdir "!MNT!" 2>nul
set "IDX=1"
for /f "tokens=2 delims=: " %%i in ('dism /English /Get-WimInfo /WimFile:"!SRC!\sources\boot.wim" ^| find "Index"') do set "IDX=%%i"
dism /English /Mount-Image /ImageFile:"!SRC!\sources\boot.wim" /Index:!IDX! /MountDir:"!MNT!" /ReadOnly >nul 2>&1
if exist "!MNT!\Windows\System32\winpeshl.ini" copy /y "!MNT!\Windows\System32\winpeshl.ini" "%STATE%\glassboot-orig-winpeshl.ini" >nul
dism /English /Unmount-Image /MountDir:"!MNT!" /Discard >nul 2>&1
call :user_hook
:mounted
echo done> "%STATE%\glassboot-mounted"
exit /b 0

rem ===========================================================================
rem Windows PE tool discs, step 3 (after the disc's wpeinit returned)
rem ===========================================================================
:toolfinish
title Glassboot
rem Give step 1 up to a minute to finish reading the disc's own startup.
for /l %%t in (1,1,60) do if not exist "%STATE%\glassboot-mounted" ping -n 2 127.0.0.1 >nul
if exist "%STATE%\glassboot-orig-winpeshl.ini" (
  set "INAPPS="
  for /f "usebackq tokens=* delims=" %%L in ("%STATE%\glassboot-orig-winpeshl.ini") do (
    set "LINE=%%L"
    if "!LINE:~0,1!"=="[" (
      set "INAPPS="
      if /i "!LINE!"=="[LaunchApps]" set "INAPPS=1"
      if /i "!LINE!"=="[LaunchApp]" set "INAPPS=1"
    ) else if defined INAPPS (
      if /i "!LINE:~0,8!"=="AppPath=" set "LINE=!LINE:~8!"
      call :run_app "!LINE!"
    )
  )
) else (
  rem No winpeshl.ini on the disc: Windows PE would have run startnet.cmd. Run it if
  rem it does more than wpeinit (which already ran).
  if exist "%SystemRoot%\System32\startnet.cmd" (
    findstr /v /i /r /c:"^ *wpeinit" /c:"^ *$" /c:"^ *rem" /c:"^ *@echo" "%SystemRoot%\System32\startnet.cmd" >nul
    if not errorlevel 1 call "%SystemRoot%\System32\startnet.cmd"
  )
)
exit /b 0

rem Run one winpeshl.ini entry, "program[, arguments]", except wpeinit (already run).
:run_app
for /f "tokens=1,* delims=," %%a in ("%~1") do (
  echo %%a | find /i "wpeinit" >nul && exit /b 0
  echo Glassboot: running %%a %%b
  call start "" /wait %%a %%b
)
exit /b 0

rem ===========================================================================
rem Shared steps
rem ===========================================================================

rem :find_data SECONDS -> DATA=X: (errorlevel 1 if not found)
:find_data
echo Glassboot: looking for the data partition...
set "DATA="
for /l %%t in (1,1,%~1) do (
  if not defined DATA (
    for %%d in (%LETTERS%) do (
      if not defined DATA if exist "%%d:\glassboot\grubenv" set "DATA=%%d:"
    )
    if not defined DATA ping -n 2 127.0.0.1 >nul
  )
)
if not defined DATA (
  echo Could not find the GLASSBOOT data partition.
  exit /b 1
)
echo   found !DATA!
exit /b 0

rem :read_options -> ISO, OPT_* from grubenv (lines "name=value"; see glassboot-windows.cfg)
:read_options
set "ISO="
set "OPT_BYPASS=0"
set "OPT_LOCAL=0"
set "OPT_UNATTEND="
set "OPT_MODE=setup"
for /f "usebackq tokens=1,* delims==" %%a in ("!DATA!\glassboot\grubenv") do (
  if "%%a"=="gb_iso" set "ISO=%%b"
  if "%%a"=="gb_win_bypass" set "OPT_BYPASS=%%b"
  if "%%a"=="gb_win_local" set "OPT_LOCAL=%%b"
  if "%%a"=="gb_win_unattend" set "OPT_UNATTEND=%%b"
  if "%%a"=="gb_win_mode" set "OPT_MODE=%%b"
)
echo   mode=!OPT_MODE! bypass=!OPT_BYPASS! local-account=!OPT_LOCAL! unattend=!OPT_UNATTEND!
exit /b 0

rem :pick_iso [interactive] -> ISO = full path (asks which ISO if grubenv had none)
:pick_iso
if defined ISO (
  set "ISO=!ISO:/=\!"
  set "ISO=!DATA!!ISO!"
)
if defined ISO if not exist "!ISO!" set "ISO="
if not defined ISO if /i "%~1"=="interactive" (
  echo.
  echo Which Windows ISO did you boot?
  set n=0
  for /r "%DATA%\" %%f in (*.iso) do (
    set /a n+=1
    set "iso_!n!=%%f"
    echo   !n!^) %%f
  )
  set /p "pick=Number: "
  for %%p in (!pick!) do set "ISO=!iso_%%p!"
)
if not defined ISO exit /b 1
exit /b 0

rem :load_drivers -> every .inf under \glassboot\drivers\ (storage/USB/network drivers
rem WinPE lacks), loaded before the ISO is attached so Setup can see the target disks.
:load_drivers
if exist "%DATA%\glassboot\drivers\" (
  for /r "%DATA%\glassboot\drivers" %%f in (*.inf) do (
    echo Glassboot: loading driver %%~nxf
    drvload "%%f" >nul
  )
)
exit /b 0

rem :attach -> SRC = drive letter of the attached ISO (errorlevel 1 on failure).
rem glassboot-attach.exe (win-helper/) uses the Virtual Disk API, which supports ISO
rem images, and prints the new drive letter. diskpart is only a fallback (VHD-oriented).
:attach
echo Glassboot: attaching !ISO!
set "ATTACH=glassboot-attach.exe"
if /i "%PROCESSOR_ARCHITECTURE%"=="x86" set "ATTACH=glassboot-attach32.exe"
set "SRC="
for /f "usebackq delims=" %%l in (`!ATTACH! "!ISO!"`) do set "SRC=%%l"
if not defined SRC (
  echo glassboot-attach could not attach the ISO; trying diskpart.
  set "DP=%TEMP%\glassboot-attach.txt"
  > "!DP!" echo select vdisk file="!ISO!"
  >>"!DP!" echo attach vdisk readonly
  diskpart /s "!DP!"
)
if defined SRC if not exist "!SRC!\sources\" set "SRC="
for /l %%t in (1,1,10) do (
  if not defined SRC (
    for %%d in (%LETTERS%) do (
      if not defined SRC if /i not "%%d:"=="!DATA!" if exist "%%d:\sources\boot.wim" set "SRC=%%d:"
    )
    if not defined SRC ping -n 2 127.0.0.1 >nul
  )
)
if not defined SRC (
  echo Could not make the ISO's files available ^(no drive with \sources\boot.wim appeared^).
  exit /b 1
)
echo   ISO is drive !SRC!
exit /b 0

rem :user_hook -> optional glassboot-user.cmd injected via \glassboot\inject\windows\
:user_hook
if exist "%SystemRoot%\System32\glassboot-user.cmd" (
  echo Glassboot: running glassboot-user.cmd
  call "%SystemRoot%\System32\glassboot-user.cmd" "!SRC!" "!DATA!"
)
exit /b 0

:shell
echo.
echo Dropping to a command prompt. Type "exit" to reboot.
cmd.exe
wpeutil reboot
