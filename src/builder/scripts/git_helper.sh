#!/bin/bash
export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin
set -e
echo "git_helper_sh: starting"
OPERATION=$1
shift
if [ "$OPERATION" == "mirror" ]; then
    REMOTE_URL=$1
    REF=$2
    HASH=$3
    MIRROR_PATH="$HOME/git-mirror/$4"
    for i in $(seq 1 60); do
        if [ -d "$MIRROR_PATH/.git" ]; then
            if git -C "$MIRROR_PATH" rev-parse -q --verify "ref-$HASH" > /dev/null; then
                exit 0
            fi
        fi
        if mkdir "$MIRROR_PATH.lock" 2>/dev/null; then
            trap 'rm -rf "$MIRROR_PATH.lock"' EXIT
            if [ -d "$MIRROR_PATH/.git" ]; then
                if git -C "$MIRROR_PATH" rev-parse -q --verify "ref-$HASH" > /dev/null; then
                    exit 0
                fi
            fi
            mkdir -p "$MIRROR_PATH"
            if [ ! -d "$MIRROR_PATH/.git" ]; then
                git init --bare "$MIRROR_PATH"
            fi
            REFSPEC="$REF:ref-$HASH"
            git -C "$MIRROR_PATH" fetch "$REMOTE_URL" "+$REFSPEC"
            exit 0
        fi
        echo "git mirror locked, waiting..."
        sleep 1
    done
    exit 1
elif [ "$OPERATION" == "checkout" ]; then
    MIRROR_PATH="$HOME/git-mirror/$1"
    BUILD_DIR=$2
    HASH=$3
    if [ ! -d "$BUILD_DIR/.git" ]; then
        rm -rf "$BUILD_DIR"
        mkdir -p "$BUILD_DIR"
        git -C "$BUILD_DIR" init
    fi
    if ! git -C "$BUILD_DIR" fetch "$MIRROR_PATH" "ref-$HASH"; then
        exit 2
    fi
    git -C "$BUILD_DIR" checkout -f "$HASH"
    git -C "$BUILD_DIR" clean -fdx
else
    exit 1
fi
echo ">>> Git helper script finished."
exit 0
