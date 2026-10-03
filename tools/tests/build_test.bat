@echo off
rem Builds and runs the offline bake test (needs a VS x64 developer environment: vcvars64.bat).
setlocal
set ROOT=%~dp0..\..
if not exist "%ROOT%\build\tests" mkdir "%ROOT%\build\tests"
cl /nologo /std:c++latest /EHsc /O2 /W4 /DNOMINMAX /I"%ROOT%\src" "%~dp0bake_test.cpp" "%ROOT%\src\Combine\Vertex.cpp" "%ROOT%\src\Combine\Bake.cpp" /Fo"%ROOT%\build\tests\\" /Fe"%ROOT%\build\tests\bake_test.exe" || exit /b 1
"%ROOT%\build\tests\bake_test.exe"
