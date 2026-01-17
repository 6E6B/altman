@echo off
setlocal EnableDelayedExpansion

cd /d "%~dp0"

del /f /s /q .\build >nul 2>nul
rmdir /s /q .\build >nul 2>nul

mkdir build >nul 2>nul

if not exist %USERPROFILE%\vcpkg\vcpkg.exe (
	echo %USERPROFILE%\vcpkg\vcpkg.exe not found.
	pause >nul 2>nul
	exit /b 1
)

%USERPROFILE%\vcpkg\vcpkg.exe install

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake -A x64 -DCMAKE_BUILD_TYPE=Release > .\build-1.log 2>&1
cmake --build build --config Release -- /m > .\build-2.log 2>&1

pause >nul 2>nul
exit /b 0
