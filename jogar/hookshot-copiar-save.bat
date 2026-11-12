@echo off
rem Copies this install's save into Hookshot's save folder; the save Hookshot had is kept aside: tools\oot\hookshot.py save.
rem Nothing leaves this PC. Hookshot's game folder can be dropped on this file; extra arguments go to the script (--help).
setlocal
cd /d "%~dp0.."
set "PY=ferramentas\python\python.exe"
if not exist "%PY%" set "PY=.venv\Scripts\python.exe"
if not exist "%PY%" (
    echo Python nao encontrado nesta instalacao: ferramentas\python, .venv / Python was not found in this install.
    pause
    exit /b 1
)
"%PY%" -I tools\oot\hookshot.py save %*
set "RC=%errorlevel%"
pause
exit /b %RC%
