@echo on
setlocal EnableDelayedExpansion
echo "git_helper_bat: starting"
set "OPERATION=%~1"
echo "OPERATION: !OPERATION!"
if /i "!OPERATION!"=="mirror" (
    set "REMOTE_URL=%~2"
    set "REF=%~3"
    set "HASH=%~4"
    set "MIRROR_PATH=%HOME%\\git-mirror\\%~5"
    echo "REMOTE_URL: !REMOTE_URL!"
    echo "REF: !REF!"
    echo "HASH: !HASH!"
    echo "MIRROR_PATH: !MIRROR_PATH!"
    :lock_wait
    mkdir "!MIRROR_PATH!.lock" 2>nul
    if errorlevel 1 (
        echo "git mirror locked, waiting..."
        timeout /t 1 /nobreak > nul
        goto :lock_wait
    )
    if exist "!MIRROR_PATH!\\.git" (
        git -C "!MIRROR_PATH!" rev-parse -q --verify "ref-!HASH!" > nul 2> nul
        if exist !MIRROR_PATH!\\.git if not errorlevel 1 (
            rmdir "!MIRROR_PATH!.lock"
            exit /b 0
        )
    )
    if not exist "!MIRROR_PATH!\\." (
    mkdir "!MIRROR_PATH!"
    )
    if not exist "!MIRROR_PATH!\\.git" (
        git init --bare "!MIRROR_PATH!"
        if errorlevel 1 (
            rmdir "!MIRROR_PATH!.lock"
            exit /b 1
        )
    )
    set "REFSPEC=!REF!:ref-!HASH!"
    echo "REFSPEC: !REFSPEC!"
    git -C "!MIRROR_PATH!" fetch "!REMOTE_URL!" "!REFSPEC!" 2>&1
    if !ERRORLEVEL! neq 0 (
        echo "git fetch failed with errorlevel !ERRORLEVEL!"
        rmdir "!MIRROR_PATH!.lock"
        exit /b 1
    )
    rmdir "!MIRROR_PATH!.lock"
    exit /b 0
)
if /i "!OPERATION!"=="checkout" (
    set "MIRROR_PATH=%HOME%\\git-mirror\\%~2"
    set "BUILD_DIR=%~3"
    set "HASH=%~4"
    echo "MIRROR_PATH: !MIRROR_PATH!"
    echo "BUILD_DIR: !BUILD_DIR!"
    echo "HASH: !HASH!"
    if not exist "!BUILD_DIR!\\.git" (
        if exist "!BUILD_DIR!\\" rmdir /s /q "!BUILD_DIR!"
        mkdir "!BUILD_DIR!"
        git -C "!BUILD_DIR!" init
        if errorlevel 1 exit /b 1
    )
    git -C "!BUILD_DIR!" fetch "!MIRROR_PATH!" "ref-!HASH!"
    if errorlevel 1 exit /b 2
    git -C "!BUILD_DIR!" checkout -f "!HASH!"
    if errorlevel 1 exit /b 1
    echo ">>> Git helper script finished."
    exit /b 0
)
exit /b 1
