#!/bin/bash

export PATH=/usr/local/bin:$PATH

set -e

OPERATION=$1
shift

if [ "$OPERATION" == "mirror" ]; then
    REMOTE_URL=$1
    REF=$2
    HASH=$3
    MIRROR_PATH=$4

    if [ ! -d "$MIRROR_PATH" ]; then
        echo ">>> Initializing new mirror at $MIRROR_PATH"
        git init --bare "$MIRROR_PATH"
    fi

    echo ">>> Fetching from $REMOTE_URL into $MIRROR_PATH"
    REFSPEC="$REF:ref-$HASH"
    git -C "$MIRROR_PATH" fetch "$REMOTE_URL" "$REFSPEC"

elif [ "$OPERATION" == "checkout" ]; then
    MIRROR_PATH=$1
    BUILD_DIR=$2
    HASH=$3

    echo ">>> Removing old build directory $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    # This is needed because the parent of BUILD_DIR might not exist yet
    mkdir -p "$(dirname "$BUILD_DIR")"

    echo ">>> Cloning from local mirror $MIRROR_PATH into $BUILD_DIR"
    git clone --local "$MIRROR_PATH" "$BUILD_DIR"

    echo ">>> Checking out commit $HASH"
    git -C "$BUILD_DIR" checkout "$HASH"

else
    echo "Unknown operation: $OPERATION"
    exit 1
fi

exit 0
