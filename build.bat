setlocal enableextensions
@echo on

rem Go home.
cd %~dp0

set PGO_OPT=
set ARCH_OPT=-x64
set REBUILD_OPT=

:CheckOpts
if "%~1" EQU "-x86" (set ARCH_OPT=-x86) && shift && goto CheckOpts
if "%~1" EQU "-x64" (set ARCH_OPT=-x64) && shift && goto CheckOpts
if "%~1" EQU "--pgo" (set PGO_OPT=--pgo) && shift && goto CheckOpts
if "%~1" EQU "-r" (set REBUILD_OPT=-r) && shift && goto CheckOpts

set NUGET_PYTHON_PACKAGE_NAME=python
set ARCH_NAME=amd64
if "%ARCH_OPT%" EQU "-x86" (
    set NUGET_PYTHON_PACKAGE_NAME=pythonx86
    set ARCH_NAME=win32
)


rem Remove old output
del /S /Q output nuget-result-%NUGET_PYTHON_PACKAGE_NAME% >nul

move .\Lib\pip.py .\Lib\pip.py.bak

mkdir Embedded\embed_data\C\vfs\ssl 2>nul

call "PCBuild\find_python.bat" "%PYTHON%"

if NOT DEFINED PYTHON (
    echo Python 3.6+ could not be found. Please make sure an existing Python version is available for use. && exit /B 1
)

%PYTHON% -c "import urllib.request; open('Embedded/embed_data/C/vfs/ssl/cert.pem', 'wb').write(urllib.request.urlopen('https://mkcert.org/generate/').read().decode('utf-8').encode('ascii', errors='backslashreplace'))"

%PYTHON% Lib\mkembeddata.py Embedded Embedded\embed_data

cl /c /Zi /FoEmbedded\mp_embed.obj Embedded\mp_embed.c /IInclude
cl /c /FoEmbedded\mp_embed_data.obj Embedded\mp_embed_data.c

lib /OUT:Embedded\mp_embed.lib Embedded\mp_embed.obj Embedded\mp_embed_data.obj


rem Build with nuget, it solves the directory structure for us.
call .\Tools\nuget\build.bat %ARCH_OPT% %PGO_OPT% %REBUILD_OPT%
if %errorlevel% neq 0 exit /b %errorlevel%

rem Install with nuget into a build folder
.\externals\windows-installer\nuget\nuget.exe install %NUGET_PYTHON_PACKAGE_NAME% -Source %~dp0\PCbuild\%ARCH_NAME% -OutputDirectory nuget-result-%NUGET_PYTHON_PACKAGE_NAME%

move .\Lib\pip.py.bak .\Lib\pip.py

rem Move the standalone build result to "output". TODO: Version number could be queried here
rem from the Python binary built, or much rather we do not use one in the nuget build at all.

set OUTPUT_DIR=output
set SRC_TOOLS_DIR=nuget-result-%NUGET_PYTHON_PACKAGE_NAME%\%NUGET_PYTHON_PACKAGE_NAME%.3.13.9\tools
set SRC_LIB_DIR=%%d\amd64
if "%ARCH_OPT%" EQU "-x86" (
    set OUTPUT_DIR=output32
    set SRC_LIB_DIR=%%d\win32
)

move %SRC_TOOLS_DIR%\Lib\pip.py.bak %SRC_TOOLS_DIR%\Lib\pip.py
copy %SRC_TOOLS_DIR%\python313.lib %SRC_TOOLS_DIR%\libs\python313.lib

xcopy /i /q /s /y %SRC_TOOLS_DIR% %OUTPUT_DIR%

for /d %%d in (externals\openssl*) do (
   xcopy /i /q /s /y %SRC_LIB_DIR%\*.lib %OUTPUT_DIR%\dependency_libs\openssl\lib && xcopy /i /q /s /y %SRC_LIB_DIR%\include %OUTPUT_DIR%\dependency_libs\openssl\include 
)

for /d %%d in (externals\libffi*) do (
   xcopy /i /q /s /y %SRC_LIB_DIR%\include %OUTPUT_DIR%\dependency_libs\libffi\include
)

mkdir %OUTPUT_DIR%\Embedded
xcopy /i /q /s /y Embedded\embed_data %OUTPUT_DIR%\Embedded\embed_data
copy Embedded\mp_embed.obj %OUTPUT_DIR%\Embedded\mp_embed.obj
copy Embedded\mp_embed.lib %OUTPUT_DIR%\libs\mp_embed.lib

%OUTPUT_DIR%\python.exe -m rebuildpython
%OUTPUT_DIR%\python.exe -c "import __mp__.packaging; __mp__.packaging.install_build_tool('clang')"

echo "Ok, MonolithPy now lives in %OUTPUT_DIR% folder"

endlocal

