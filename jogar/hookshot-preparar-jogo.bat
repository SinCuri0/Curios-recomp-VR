@echo off
rem Makes the folder with romfs and exefs that Hookshot Builder takes ("romfs and exefs folders") from this install's
rem game files: tools\oot\hookshot.py romfs. Nothing leaves this PC. Extra arguments go to the script (--help lists them).
setlocal
cd /d "%~dp0.."
set "PY=ferramentas\python\python.exe"
if not exist "%PY%" set "PY=.venv\Scripts\python.exe"
if not exist "%PY%" (
    echo Python nao encontrado nesta instalacao: ferramentas\python, .venv / Python was not found in this install.
    pause
    exit /b 1
)
"%PY%" -I tools\oot\hookshot.py romfs %*
set "RC=%errorlevel%"
pause
exit /b %RC%
