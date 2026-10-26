@echo off
rem Builds build\Sq_Repextend.aex
rem Needs the After Effects SDK. Either set AE_SDK_ROOT, or unzip the SDK anywhere under .\SDK
setlocal enabledelayedexpansion
pushd "%~dp0"

rem ---- locate the SDK (the folder that contains Headers\AE_Effect.h) ----
set "SDK_EX="
if defined AE_SDK_ROOT (
	if exist "%AE_SDK_ROOT%\Examples\Headers\AE_Effect.h" set "SDK_EX=%AE_SDK_ROOT%\Examples"
	if exist "%AE_SDK_ROOT%\Headers\AE_Effect.h" set "SDK_EX=%AE_SDK_ROOT%"
)
if not defined SDK_EX (
	for /f "delims=" %%F in ('dir /s /b "SDK\AE_Effect.h" 2^>nul') do (
		if not defined SDK_EX (
			set "HDR=%%~dpF"
			for %%P in ("!HDR:~0,-1!\..") do set "SDK_EX=%%~fP"
		)
	)
)
if not defined SDK_EX (
	echo [ERROR] After Effects SDK not found.
	echo         Unzip it under "%~dp0SDK" or set AE_SDK_ROOT.
	goto :fail
)
echo SDK: %SDK_EX%

set "PIPLTOOL="
for /f "delims=" %%F in ('dir /s /b "%SDK_EX%\PiPLTool.exe" 2^>nul') do if not defined PIPLTOOL set "PIPLTOOL=%%F"
if not defined PIPLTOOL (
	echo [ERROR] PiPLTool.exe not found in the SDK.
	goto :fail
)

rem ---- MSVC environment ----
where cl >nul 2>nul
if errorlevel 1 (
	set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
	for /f "delims=" %%I in ('"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSDIR=%%I"
	if not defined VSDIR (
		echo [ERROR] Visual Studio C++ build tools not found.
		goto :fail
	)
	call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :fail
)

if not exist build mkdir build

set INCLUDES=/I "src" /I "%SDK_EX%\Headers" /I "%SDK_EX%\Headers\SP" /I "%SDK_EX%\Headers\Win" /I "%SDK_EX%\Util" /I "%SDK_EX%\Resources"

rem ---- PiPL resource (same steps as the SDK sample projects) ----
cl /nologo %INCLUDES% /EP "src\SqRepextend_PiPL.r" > "build\SqRepextend_PiPL.rr" || goto :fail
"%PIPLTOOL%" "build\SqRepextend_PiPL.rr" "build\SqRepextend_PiPL.rrc" || goto :fail
cl /nologo /D "MSWindows" /EP "build\SqRepextend_PiPL.rrc" > "build\SqRepextend_PiPL.rc" || goto :fail
rc /nologo /fo "build\SqRepextend_PiPL.res" "build\SqRepextend_PiPL.rc" || goto :fail

rem ---- plug-in ----
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /utf-8 ^
	/D MSWindows /D WIN32 /D _WINDOWS /D NDEBUG /D _CRT_SECURE_NO_WARNINGS ^
	%INCLUDES% /Fo"build\\" ^
	"src\SqRepextend.cpp" "build\SqRepextend_PiPL.res" ^
	/LD /Fe"build\Sq_Repextend.aex" ^
	/link /DLL /NOLOGO || goto :fail

echo.
echo Built: %~dp0build\Sq_Repextend.aex
popd
exit /b 0

:fail
echo Build failed.
popd
exit /b 1
