@echo off
REM Сборка тестового harness (x64). Запускать из корня проекта.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cl /nologo /EHsc /utf-8 /I include test\testharness.cpp /Fe:test\testharness.exe /Fo:test\testharness.obj
