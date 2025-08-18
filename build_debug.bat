@echo off
setlocal
set RL=C:\raylib

if exist build rmdir /s /q build
cmake -S . -B build -G "Ninja" -DCMAKE_PREFIX_PATH=%RL% -DCMAKE_BUILD_TYPE=Debug -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebugDLL
cmake --build build

@REM echo.
@REM echo Running debug build (will show exit code)...
@REM build\create.exe
@REM echo.
@REM echo Exit code: %errorlevel%

copy /Y build\*.exe .
