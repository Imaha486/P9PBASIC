@echo off
rem NOT SUPPORTED in P8PBASIC -- inherited unchanged from P9PBASIC.
rem P8PBASIC's interpreter calls the Z80 vessel throughout and the
rem vessel has no 16-bit DOS target, so this build cannot link.
rem Use build-host.sh / build-embed.sh. See docs/APPROXIMATIONS.md
rem audit item G-5.
echo build.bat: the DOS build is a P9PBASIC target and is not supported in P8PBASIC.
echo Use build-host.sh or build-embed.sh instead.
exit /b 1
rem P9PBASIC.EXE build script for Open Watcom 2.0 on Windows
rem   https://github.com/open-watcom/open-watcom-v2
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
set PATH=%WATCOM%\BINNT64;%WATCOM%\BINNT;%PATH%
set INCLUDE=%WATCOM%\H
if not exist obj mkdir obj
set CF=-za99 -q -bt=dos -ml -0 -fpc -s -ox -w3 -zq
for %%f in (main exec eval var num token print screen graph fileio) do (
  wcc %CF% -fo=obj\%%f.obj src\%%f.c
  if errorlevel 1 goto err
)
wcc %CF% -zu -fo=obj\hal98.obj src\hal98.c
if errorlevel 1 goto err
wlink system dos option quiet option stack=16384 name P9PBASIC.EXE file { obj\main.obj obj\exec.obj obj\eval.obj obj\var.obj obj\num.obj obj\token.obj obj\print.obj obj\screen.obj obj\graph.obj obj\fileio.obj obj\hal98.obj }
if errorlevel 1 goto err
echo OK: P9PBASIC.EXE
goto end
:err
echo BUILD FAILED
:end
