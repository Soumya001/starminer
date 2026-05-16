@echo off
title StarMiner Worker
echo.
echo  ========================================
echo    STARMINER WORKER - Windows
echo  ========================================
echo.

REM Check for Python
python --version >nul 2>&1
if errorlevel 1 (
    echo  ERROR: Python is not installed or not in PATH.
    echo  Please install Python 3 from https://python.org
    echo.
    pause
    exit /b 1
)

REM Check for worker script
if not exist "starminer-worker.py" (
    echo  Downloading worker script...
    powershell -Command "Invoke-WebRequest -Uri 'https://starnetlive.space/download/starminer-worker.py' -OutFile 'starminer-worker.py'"
)

echo  Starting StarMiner Worker...
echo  Pool: https://starnetlive.space
echo.

REM Ask for worker name if not set
set /p WORKER="Enter your Bitcoin address (worker name): "
if "%WORKER%"=="" set WORKER=worker-%COMPUTERNAME%

set /p GPUS="GPU IDs (comma-separated, default=0): "
if "%GPUS%"=="" set GPUS=0

python starminer-worker.py --pool https://starnetlive.space --worker %WORKER% --gpus %GPUS%

pause
