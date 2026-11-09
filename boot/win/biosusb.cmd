@echo off
rem BIOS-USB Windows PE helper.
rem
rem wimboot injects this file (and winpeshl.ini, which runs it) into X:\Windows\System32
rem of the installer's boot.wim. Windows PE cannot see the ISO GRUB booted from, so we:
rem   1. find the BIOSUSB data partition (it has \biosusb\grubenv),
rem   2. read the ISO path GRUB saved there (falls back to a menu),
rem   3. attach the ISO with diskpart (built-in virtual disk support),
rem   4. start Windows Setup from the attached ISO.
setlocal EnableExtensions EnableDelayedExpansion
title BIOS-USB
set "LETTERS=C D E F G H I J K L M N O P Q R S T U V W Y Z"

echo BIOS-USB: looking for the data partition...
set "DATA="
for /l %%t in (1,1,20) do (
  if not defined DATA (
    for %%d in (%LETTERS%) do (
      if not defined DATA if exist "%%d:\biosusb\grubenv" set "DATA=%%d:"
    )
    if not defined DATA ping -n 2 127.0.0.1 >nul
  )
)
if not defined DATA (
  echo Could not find the BIOSUSB data partition.
  goto :shell
)
echo   found !DATA!

rem ---- ISO chosen in GRUB (grubenv line "bu_iso=/path/to/file.iso")
set "ISO="
for /f "usebackq tokens=1,* delims==" %%a in ("!DATA!\biosusb\grubenv") do (
  if "%%a"=="bu_iso" set "ISO=%%b"
)
if defined ISO (
  set "ISO=!ISO:/=\!"
  set "ISO=!DATA!!ISO!"
)
if defined ISO if not exist "!ISO!" set "ISO="

if not defined ISO (
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
if not defined ISO goto :shell

echo BIOS-USB: attaching !ISO!
set "DP=%TEMP%\biosusb-attach.txt"
> "!DP!" echo select vdisk file="!ISO!"
>>"!DP!" echo attach vdisk readonly
diskpart /s "!DP!"
if errorlevel 1 (
  echo diskpart could not attach the ISO.
  goto :shell
)

rem ---- find the attached ISO's drive letter
set "SRC="
for /l %%t in (1,1,10) do (
  if not defined SRC (
    for %%d in (%LETTERS%) do (
      if not defined SRC if /i not "%%d:"=="!DATA!" if exist "%%d:\setup.exe" if exist "%%d:\sources\boot.wim" set "SRC=%%d:"
    )
    if not defined SRC ping -n 2 127.0.0.1 >nul
  )
)
if not defined SRC (
  echo The ISO was attached but no drive letter with setup.exe appeared.
  goto :shell
)

echo BIOS-USB: starting Windows Setup from !SRC!
"!SRC!\setup.exe"
wpeutil reboot
exit /b 0

:shell
echo.
echo Dropping to a command prompt. Type "exit" to reboot.
cmd.exe
wpeutil reboot
