@echo off
setlocal EnableExtensions DisableDelayedExpansion

if /I not "%OS%"=="Windows_NT" goto :unsupported_platform
set "BUILD_HOST_ARCH=%PROCESSOR_ARCHITECTURE%"
if defined PROCESSOR_ARCHITEW6432 set "BUILD_HOST_ARCH=%PROCESSOR_ARCHITEW6432%"
if /I not "%BUILD_HOST_ARCH%"=="AMD64" goto :unsupported_platform
if not "%~1"=="" goto :usage

set "BUILD_CMAKE="
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined BUILD_CMAKE set "BUILD_CMAKE=%%I"
if defined BUILD_CMAKE goto :configure

set "BUILD_VSWHERE_DIR=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%BUILD_VSWHERE_DIR%\vswhere.exe" goto :missing_cmake
pushd "%BUILD_VSWHERE_DIR%"
if errorlevel 1 goto :missing_cmake
set "BUILD_VS_DIR="
for /f "delims=" %%I in ('.\vswhere.exe -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "BUILD_VS_DIR=%%I"
popd
if not defined BUILD_VS_DIR goto :missing_cmake
set "BUILD_CMAKE=%BUILD_VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%BUILD_CMAKE%" goto :missing_cmake

:configure
pushd "%~dp0"
if errorlevel 1 goto :missing_workspace

echo Configurando o aplicativo para Windows x64...
"%BUILD_CMAKE%" -S . -B build/windows-x64 -G "Visual Studio 17 2022" -A x64 -DCMAKE_SYSTEM_VERSION=10.0.26100.0 -DBUILD_TESTING=OFF
if errorlevel 1 goto :build_failed

echo Compilando YourotsCapture em Release x64...
"%BUILD_CMAKE%" --build build/windows-x64 --config Release --target YourotsCapture --parallel
if errorlevel 1 goto :build_failed

echo.
echo Compilacao concluida: "%CD%\build\windows-x64\Release\YourotsCapture.exe"
popd
exit /b 0

:build_failed
set "BUILD_EXIT_CODE=%ERRORLEVEL%"
echo [ERRO] A compilacao falhou. Confira as mensagens acima.
popd
exit /b %BUILD_EXIT_CODE%

:unsupported_platform
echo [ERRO] Este script suporta apenas Windows x64, com processador AMD64/Intel64.
exit /b 1

:missing_cmake
echo [ERRO] CMake nao encontrado no PATH nem no Visual Studio 2022.
echo Instale CMake 3.28 ou superior e as ferramentas C++ do Visual Studio 2022.
exit /b 1

:missing_workspace
echo [ERRO] Nao foi possivel acessar a pasta do projeto.
exit /b 1

:usage
echo Uso: compilar_windows_x64.cmd
echo O script compila somente o aplicativo em Release para Windows x64.
exit /b 1
