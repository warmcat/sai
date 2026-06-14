#!/bin/sh

set -e

if [ -z "$1" ] ; then
	echo "Usage $0 username"
	exit 1
fi

if [ "`whoami`" != "root" ] ; then
	echo "Run as root"
	exit 1
fi

USERNAME="$1"
# Robust way to get the user's home directory on both Linux and macOS
export BD="$(sudo -H -u "$USERNAME" sh -c 'cd && pwd')"

# change to $1 user
sudo -u "$USERNAME" sh -c "
set -e

echo sanity=2 basedir=${BD}

mkdir -p \"${BD}/libwebsockets/build\" && \
cd \"${BD}/libwebsockets/build\" && \
git fetch https://libwebsockets.org/repo/libwebsockets +_temp:m && \
git reset --hard m && \
make -j12
"

# return back to root
cd "${BD}/libwebsockets/build"
make -j12 install

# change back to $1 user
sudo -u "$USERNAME" sh -c "
set -e

cd \"${BD}/sai/build\" && \
git fetch https://warmcat.com/repo/sai +_temp:m && \
git reset --hard m && \
make -j12
"

# return back to root
cd "${BD}/sai/build"
make -j12 install

OS=$(uname -s)
if [ "$OS" = "Linux" ]; then
	systemctl restart sai-builder
elif [ "$OS" = "Darwin" ]; then
	launchctl unload /Library/LaunchDaemons/com.warmcat.sai-builder.plist
	launchctl load /Library/LaunchDaemons/com.warmcat.sai-builder.plist
else
	echo "Unknown OS: $OS. Cannot restart sai-builder."
fi

exit 0
