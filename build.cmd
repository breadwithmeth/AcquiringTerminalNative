@echo off
rem ============================================================
rem Сборка AcquiringTerminalNative (x64 + x86, Release)
rem Требуются VS 2022 Build Tools с рабочей нагрузкой C++.
rem
rem Необязательный аргумент — toolset, если v141_xp не установлен:
rem   build.cmd v143
rem ============================================================
setlocal
cd /d "%~dp0"

set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -products * -latest -requires Microsoft.Component.MSBuild -find MSBuild\Current\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
)

if "%MSBUILD%"=="" if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
)

if "%MSBUILD%"=="" (
    echo MSBuild не найден. Установите VS 2022 Build Tools с рабочей нагрузкой C++.
    exit /b 1
)

rem Необязательное переопределение toolset
set "TOOLSET="
if not "%~1"=="" set "TOOLSET=/p:PlatformToolset=%~1"

rem Windows SDK: если платформенная версия не задана в проекте, берём последнюю
for /f "delims=" %%d in ('dir /b /ad "C:\Program Files (x86)\Windows Kits\10\Include" 2^>nul') do set "SDK=%%d"
set "SDKARG="
if not "%SDK%"=="" set "SDKARG=/p:WindowsTargetPlatformVersion=%SDK%"

echo MSBuild: %MSBUILD%
if not "%TOOLSET%"=="" echo Toolset: %~1
echo.

echo === Сборка x64 (Release) ===
"%MSBUILD%" "%~dp0AcquiringTerminalNative.sln" /p:Configuration=Release /p:Platform=x64 %TOOLSET% %SDKARG% /m /v:m || goto :fail
echo.
echo === Сборка x86 (Release) ===
"%MSBUILD%" "%~dp0AcquiringTerminalNative.sln" /p:Configuration=Release /p:Platform=x86 %TOOLSET% %SDKARG% /m /v:m || goto :fail

echo.
echo Готово. Артефакты:
dir /b "1C\Release\*.dll"
exit /b 0

:fail
echo СБОРКА ЗАВЕРШИЛАСЬ С ОШИБКОЙ
exit /b 1
