@echo off
rem ============================================================================
rem  Network serial terminal (Windows)  --  see README, section "Network serial"
rem
rem  Use the TCP port exposed by esp32c3-wifi-serial as a local serial port.
rem  Zero deps: Python standard library only, no pip install needed.
rem
rem  !!! THIS FILE IS INTENTIONALLY ASCII-ONLY !!!
rem  cmd.exe reads .bat using the ANSI code page (936/GBK on zh-CN Windows), so a
rem  UTF-8 file with Chinese comments gets decoded as GBK, the byte stream falls
rem  out of sync, and lines turn into bogus commands ('xxx' is not recognized).
rem  All Chinese output is printed by the Python side instead -- the Windows
rem  console handles UTF-8 via PEP 528, independent of the code page.
rem
rem  Usage:
rem      net-term.bat 192.168.0.239 2333                     interactive
rem      net-term.bat 192.168.0.239 2333 --no-crlf           raw bytes, no CRLF
rem      net-term.bat 192.168.0.239 2333 --send AT --wait 2  send then exit
rem      net-term.bat 192.168.0.239:2333                     port inside the IP
rem      net-term.bat                                        prompt for the IP
rem
rem  Why not telnet / nc:
rem      Windows does not ship TelnetClient by default and nc / ncat / PuTTY may
rem      be absent too. More importantly a real telnet client must NOT connect
rem      to the TCP-mode port: firmware only filters IAC in NET_TELNET mode, so
rem      telnet's FF FD 01... negotiation bytes would be written straight into
rem      the downstream serial port. This script never sends IAC.
rem      To use a telnet client, switch "network mode" to Telnet in the web
rem      config page first (the firmware then listens on port 23).
rem
rem  Careful with empty arguments:
rem      cmd can neither quote-check nor type-check anything. If the IP is an
rem      empty string, "python script.py %IP% 2333" silently becomes
rem      "python script.py 2333" -- the port slides into the host slot and you
rem      get a bogus "could not connect 2333:2333". So: args are collected with
rem      original quoting preserved, the prompt result is validated and
rem      re-asked, and nothing is forwarded until we know it is not empty.
rem ============================================================================
setlocal
cd /d "%~dp0"

set "ARGS="
set "GOT="

rem ---- collect argv[1..n] verbatim (quotes kept, so --send "AT+GMR x" survives)
:collect
if "%~1"=="" goto collected
set "ARGS=%ARGS% %1"
set "GOT=1"
shift
goto collect

:collected
if defined GOT goto havehost

:ask
rem A bare Enter means "variable stays empty" -- that is exactly the trap above.
set /p "ARGS=Device IP, e.g. 192.168.0.239: "
if not defined ARGS goto badinput

rem Trim spaces/tabs; a string of spaces counts as empty too.
for /f "tokens=* delims= " %%a in ("%ARGS%") do set "ARGS=%%a"
if not defined ARGS goto badinput

:havehost
rem ---- find any usable python3 (this script needs no third-party packages) ----
set "PY="
if exist "%~dp0.venv\Scripts\python.exe" set "PY=%~dp0.venv\Scripts\python.exe"

if not defined PY for %%d in (
  "D:\Espressif\python_env\idf5.5_py3.13_env"
  "D:\Espressif\python_env\idf5.4_py3.11_env"
  "D:\Espressif\python_env\idf5.3_py3.11_env"
) do (
  if not defined PY if exist "%%~d\Scripts\python.exe" set "PY=%%~d\Scripts\python.exe"
)

rem python from PATH: skip the WindowsApps execution-alias stubs (they open the Store)
if not defined PY for %%c in (python python3) do (
  if not defined PY for /f "delims=" %%p in ('where %%c 2^>nul ^| findstr /v /i "WindowsApps"') do set "PY=%%p"
)

if not defined PY (
  echo.
  echo [ERROR] No python3 found. Install Python 3 and tick "Add to PATH";
  echo         no extra packages are required. Or use the one bundled with
  echo         ESP-IDF: D:\Espressif\python_env\*\Scripts\python.exe
  echo.
  pause
  exit /b 1
)

"%PY%" "%~dp0tools\tcp-term.py" %ARGS%
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
  echo.
  echo [exit code %RC%]   ^(2 = could not connect to the target, 3 = bad arguments^)
)
pause
rem No explicit endlocal here: %RC% must still be in scope when this line runs,
rem and exit /b pops the setlocal on its own anyway.
exit /b %RC%

:badinput
echo.
echo [ERROR] No device IP given.
echo         Usage: net-term.bat 192.168.0.239 2333
echo         ^(find the IP on the web status page: net.ip / STA IP^)
echo.
pause
endlocal
exit /b 1
