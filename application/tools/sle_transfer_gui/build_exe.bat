@echo off
setlocal
cd /d "%~dp0"

py -m pip install -r requirements.txt pyinstaller
if errorlevel 1 goto :failed

py -m PyInstaller --noconfirm --clean --onefile --windowed ^
  --name SLE-5Hop-Transfer ^
  app.py
if errorlevel 1 goto :failed

echo.
echo Build complete: %CD%\dist\SLE-5Hop-Transfer.exe
pause
exit /b 0

:failed
echo.
echo Build failed.
pause
exit /b 1
