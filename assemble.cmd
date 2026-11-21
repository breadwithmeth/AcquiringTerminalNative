@echo off
rem ============================================================
rem Упаковка внешней компоненты в zip для 1С
rem («Драйверы оборудования» / ПодключитьВнешнююКомпоненту)
rem Результат: AcquiringTerminalNative.zip (MANIFEST.xml + INFO.XML + DLL)
rem Сначала выполните сборку: build.cmd
rem ============================================================
setlocal
cd /d "%~dp0"

if not exist "1C\Release\AcquiringTerminalNative_64.dll" (
    echo Сначала выполните сборку: build.cmd
    exit /b 1
)

set "STAGE=%TEMP%\AcquiringTerminalNative_pack"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"

copy /y "1C\Release\MANIFEST.xml" "%STAGE%\" >nul
copy /y "1C\Release\INFO.XML" "%STAGE%\" >nul
copy /y "1C\Release\AcquiringTerminalNative_32.dll" "%STAGE%\" >nul
copy /y "1C\Release\AcquiringTerminalNative_64.dll" "%STAGE%\" >nul

if exist AcquiringTerminalNative.zip del /q AcquiringTerminalNative.zip

powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath 'AcquiringTerminalNative.zip'" || exit /b 1

echo Готово: AcquiringTerminalNative.zip
dir AcquiringTerminalNative.zip | find "AcquiringTerminalNative.zip"
endlocal
