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

rem mp_embed.c #includes <zstd.h>, so we need zstd's headers before the
rem cl below. Calling PCBuild\get_externals.bat would also work, but it
rem also tries to fetch llvm (which 404s on MonolithPy-bin-deps). Just
rem download the zstd source directly - the same source PCBuild's nuget
rem step pulls down and references via $(zstdDir).
if not exist externals\zstd-1.5.7 (
    if not exist externals mkdir externals
    curl -L -o externals\zstd-1.5.7.tar.gz https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz || exit /b 1
    tar -xf externals\zstd-1.5.7.tar.gz -C externals || exit /b 1
)

cl /c /Zi /FoEmbedded\mp_embed.obj Embedded\mp_embed.c /IInclude /Iexternals\zstd-1.5.7\lib
cl /c /FoEmbedded\mp_embed_data.obj Embedded\mp_embed_data.c

rem Bundle all of zstd's source into mp_embed.lib. Every project that uses
rem pyproject.props links mp_embed.lib (via AdditionalDependencies), so
rem this guarantees ZSTD_decompress / ZSTD_isError / etc. resolve for any
rem binary that mp_embed.obj gets pulled into - notably _freeze_module,
rem which is built before the Python C extension modules. We trim the
rem matching <ClCompile> entries out of PCbuild\_zstd.vcxproj so python.exe
rem doesn't end up with two copies of zstd at link time.
if not exist Embedded\zstd_objs mkdir Embedded\zstd_objs
cl /c /MT /Zi /nologo /DZSTD_MULTITHREAD=1 ^
   /Iexternals\zstd-1.5.7\lib ^
   /Iexternals\zstd-1.5.7\lib\common ^
   /Iexternals\zstd-1.5.7\lib\compress ^
   /Iexternals\zstd-1.5.7\lib\decompress ^
   /Iexternals\zstd-1.5.7\lib\dictBuilder ^
   /FoEmbedded\zstd_objs\ ^
   externals\zstd-1.5.7\lib\common\*.c ^
   externals\zstd-1.5.7\lib\compress\*.c ^
   externals\zstd-1.5.7\lib\decompress\*.c ^
   externals\zstd-1.5.7\lib\dictBuilder\*.c
if errorlevel 1 exit /b 1

rem mp_embed.lib carries only the implementation + zstd, *not* the
rem embed data. We ship mp_embed_data.obj separately so test binaries
rem can substitute their own (otherwise linking mp_embed.lib would
rem also pull in the production blob's nuitka_embed_*).
rem mp_embed_data.lib (just the production data wrapped) is what
rem python.exe links via pyproject.props.
lib /OUT:Embedded\mp_embed.lib Embedded\mp_embed.obj Embedded\zstd_objs\*.obj
if errorlevel 1 exit /b 1
lib /OUT:Embedded\mp_embed_data.lib Embedded\mp_embed_data.obj
if errorlevel 1 exit /b 1


rem Build with nuget, it solves the directory structure for us.
call .\Tools\nuget\build.bat %ARCH_OPT% %PGO_OPT% %REBUILD_OPT%
if %errorlevel% neq 0 exit /b %errorlevel%

rem Install with nuget into a build folder
.\externals\windows-installer\nuget\nuget.exe install %NUGET_PYTHON_PACKAGE_NAME% -Source %~dp0\PCbuild\%ARCH_NAME% -OutputDirectory nuget-result-%NUGET_PYTHON_PACKAGE_NAME%

move .\Lib\pip.py.bak .\Lib\pip.py

rem Move the standalone build result to "output". TODO: Version number could be queried here
rem from the Python binary built, or much rather we do not use one in the nuget build at all.

set OUTPUT_DIR=output
rem PYTHON_NUSPEC_VERSION is determined by PCbuild's nuspec - it tracks the
rem actual Python version (e.g. 3.14.4). Discover the directory dynamically
rem so the 3.13->3.14->next port doesn't keep breaking this xcopy.
for /d %%v in (nuget-result-%NUGET_PYTHON_PACKAGE_NAME%\%NUGET_PYTHON_PACKAGE_NAME%.*) do set SRC_TOOLS_DIR=%%v\tools
set SRC_LIB_DIR=%%d\amd64
if "%ARCH_OPT%" EQU "-x86" (
    set OUTPUT_DIR=output32
    set SRC_LIB_DIR=%%d\win32
)

move %SRC_TOOLS_DIR%\Lib\pip.py.bak %SRC_TOOLS_DIR%\Lib\pip.py
rem Match the actual built python<MAJOR><MINOR>.lib (e.g. python314.lib).
for %%f in (%SRC_TOOLS_DIR%\python3*.lib) do copy "%%f" %SRC_TOOLS_DIR%\libs\%%~nxf

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
copy Embedded\mp_embed_data.lib %OUTPUT_DIR%\libs\mp_embed_data.lib

%OUTPUT_DIR%\python.exe -m rebuildpython

echo "Ok, MonolithPy now lives in %OUTPUT_DIR% folder"

endlocal

