@echo off
setlocal

set "OPERATION=%~1"
set "ARG2=%~2"
set "ARG3=%~3"
set "ARG4=%~4"
set "ARG5=%~5"

if /i "%OPERATION%"=="mirror" (
    set "REMOTE_URL=%ARG2%"
    set "REF=%ARG3%"
    set "HASH=%ARG4%"
    set "MIRROR_PATH=%ARG5%"

    if not exist "%MIRROR_PATH%\." (
        echo ">>> Initializing new mirror at %MIRROR_PATH%"
        git init --bare "%MIRROR_PATH%"
        if errorlevel 1 exit /b 1
    )

    echo ">>> Fetching from %REMOTE_URL% into %MIRROR_PATH%"
    set "REFSPEC=%REF%:ref-%HASH%"
    git -C "%MIRROR_PATH%" fetch "%REMOTE_URL%" %REFSPEC%
    if errorlevel 1 exit /b 1

    exit /b 0
)

if /i "%OPERATION%"=="checkout" (
    set "MIRROR_PATH=%ARG2%"
    set "BUILD_DIR=%ARG3%"
    set "HASH=%ARG4%"

    if exist "%BUILD_DIR%\" (
        echo ">>> Removing old build directory %BUILD_DIR%"
        rmdir /s /q "%BUILD_DIR%"
        if errorlevel 1 exit /b 1
    )

    echo ">>> Cloning from local mirror %MIRROR_PATH% into %BUILD_DIR%"
    git clone --local "%MIRROR_PATH%" "%BUILD_DIR%"
    if errorlevel 1 exit /b 1

    echo ">>> Checking out commit %HASH%"
    git -C "%BUILD_DIR%" checkout "%HASH%"
    if errorlevel 1 exit /b 1

    exit /b 0
)

echo "Unknown operation: %OPERATION%"
exit /b 1
