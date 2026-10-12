@echo off
setlocal
cd /d "%~dp0"

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo MSVC build tools not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cl /nologo /utf-8 /std:c++17 /O1 /MT /W4 /EHsc /DUNICODE /D_UNICODE hush.cpp /Fe:Hush.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF || exit /b 1
del /q hush.obj 2>nul
echo Built Hush.exe
