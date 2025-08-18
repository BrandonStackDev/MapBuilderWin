@REM cmake -S . -B build -G "Ninja" -DCMAKE_PREFIX_PATH=C:\raylib
@REM cmake --build build

@echo off
setlocal

set RL=C:\raylib

if exist build rmdir /s /q build
cmake -S . -B build -G "Ninja" -DCMAKE_PREFIX_PATH=%RL% -DCMAKE_BUILD_TYPE=Release
cmake --build build

@REM echo.
@REM echo Run:
@REM build\create.exe

copy /Y build\*.exe .


