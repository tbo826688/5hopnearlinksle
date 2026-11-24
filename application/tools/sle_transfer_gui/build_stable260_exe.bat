@echo off
setlocal
cd /d "%~dp0"

py -m pip install -r requirements.txt pyinstaller
if errorlevel 1 goto :failed

py -m PyInstaller --noconfirm --clean SLE-5Hop-Transfer-Stable260.spec
if errorlevel 1 goto :failed

echo.
echo Build complete: %CD%\dist\SLE-5Hop-Transfer-Stable260.exe
pause
exit /b 0

:failed
echo.
echo Build failed.
pause
exit /b 1
