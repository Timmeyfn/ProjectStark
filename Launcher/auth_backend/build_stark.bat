@echo off
REM Builds lawinserver.exe for the Project Stark Launcher.
REM Needs Node.js (LTS) from https://nodejs.org
cd /d "%~dp0"
call npm install || goto :error
call npx --yes @yao-pkg/pkg index.js --targets node20-win-x64 --output lawinserver.exe || goto :error
copy /y lawinserver.exe "..\gui\assets\backend\lawinserver.exe" >nul || goto :error
echo.
echo Done! lawinserver.exe was rebuilt and copied into the launcher.
pause
exit /b 0
:error
echo.
echo Something went wrong, screenshot this window for Claude.
pause
exit /b 1
