@echo off
rem OMKit firmware tool for Windows: finds Python 3.9 or newer and runs omkit_firmware.py from this folder.
rem Double-click it, or run it in a terminal (arguments are passed on, e.g. --firmware FILE).
setlocal
cd /d "%~dp0"
title OMKit firmware tool
set "PY="
py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)" >nul 2>nul
if not errorlevel 1 set "PY=py -3"
if defined PY goto run
python -c "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)" >nul 2>nul
if not errorlevel 1 set "PY=python"
if defined PY goto run
python3 -c "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)" >nul 2>nul
if not errorlevel 1 set "PY=python3"
if not defined PY goto nopython

:run
%PY% "%~dp0omkit_firmware.py" %*
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" echo Finished. Where the files are: see the lines above.
if not "%RC%"=="0" echo The tool stopped, see the messages above.
pause
exit /b %RC%

:nopython
echo Python 3.9 or newer was not found on this computer.
echo.
echo Install Python, then run this file again:
echo   - from https://www.python.org/downloads/ - in the installer tick "Add python.exe to PATH";
echo   - or in a terminal: winget install Python.Python.3.12
echo.
pause
exit /b 1
