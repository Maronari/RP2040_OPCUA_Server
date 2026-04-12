@echo off
REM Export OPC UA nodeset from RP2350 server
REM Usage: export_nodeset.bat <server_ip> [namespace_index] [username] [password]
REM Example: export_nodeset.bat 192.168.1.100 1 admin secret
REM If username is provided without password, the Python script will prompt.

set SERVER_IP=%1
set NS_INDEX=%2
set USERNAME=%3
set PASSWORD=%4

if "%SERVER_IP%"=="" (
    echo Usage: export_nodeset.bat ^<server_ip^> [namespace_index] [username] [password]
    echo Example: export_nodeset.bat 192.168.1.100 1 admin secret
    exit /b 1
)

if "%NS_INDEX%"=="" set NS_INDEX=1

set SERVER_URL=opc.tcp://%SERVER_IP%:4840

set EXTRA_ARGS=
if not "%USERNAME%"=="" set EXTRA_ARGS=%EXTRA_ARGS% --username %USERNAME%
if not "%PASSWORD%"=="" set EXTRA_ARGS=%EXTRA_ARGS% --password %PASSWORD%

echo Exporting namespace %NS_INDEX% from %SERVER_URL%...

C:\Python312\python.exe "%~dp0export_nodeset.py" %SERVER_URL% %NS_INDEX% %EXTRA_ARGS%

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ERROR: Export failed. Make sure:
    echo   1. RP2350 server is running and accessible
    echo   2. asyncua is installed: C:\Python312\python.exe -m pip install --user asyncua
    echo   3. If protected nodes are hidden, export under admin credentials
    exit /b 1
)

echo.
echo Export completed successfully!
