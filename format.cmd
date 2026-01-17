@echo off
setlocal EnableDelayedExpansion

cd /d "%~dp0"

where clang-format >nul 2>nul
if errorlevel 1 (
	echo clang-format not found in PATH.
	echo Please install clang-format or add it to your PATH.
	pause >nul 2>nul
	exit /b 1
)

echo Formatting all source files...

for /r %%f in (*.cpp *.h *.c *.hpp *.cc *.cxx) do (
	echo Formatting: "%%f"
	clang-format --style="file:./.clang-format" -i "%%f"
)

echo All files formatted successfully
pause >nul 2>nul
exit /b 0
