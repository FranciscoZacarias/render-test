@echo off
setlocal EnableDelayedExpansion

set compiler=cl
set compiler_flags=/link /incremental:no 
set build_dir=build

set main_program=entry_point.c
set meta_program=metaprogram.c

set external_include=/I"..\src" /I"..\src\fzac" /I"..\src\fzac\modules"

if "%1"=="" (
  set target=entry
  set build_type=debug
) else (
  set target=%1
  set build_type=debug
)

if "%2" NEQ "" (
  set build_type=%2
)

set common_flags=/nologo /FC /W4 /WX

if /I "%build_type%"=="debug" (
  set cl_flags=%common_flags% /Zi /Od /DDEBUG
  set suffix=_debug
) else if /I "%build_type%"=="release" (
  set cl_flags=%common_flags% /O2 /DNDEBUG
  set suffix=
) else (
  echo Unknown build type: %build_type%
  exit /b 1
)

if not exist %build_dir% mkdir %build_dir%
pushd %build_dir%

if /I "%target%"=="entry" goto build_entry
if /I "%target%"=="metaprogram" goto build_metaprogram

echo Unknown target: %target%
goto done

:build_entry
echo Compiling ENTRY (%build_type%)
%compiler% ..\src\%main_program% %cl_flags% %external_include% /Fe"renderer%suffix%.exe" %compiler_flags%
goto done

:build_metaprogram
echo Compiling METAPROGRAM (%build_type%)
%compiler% ..\src\%meta_program% %cl_flags% %external_include% /Fe"metaprogram%suffix%.exe" %compiler_flags%
goto done

:done
popd
