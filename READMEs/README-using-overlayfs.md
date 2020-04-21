# Overview

This shows how to create the layered nspawn overlay fs dirs
for use with sai-builder, from scratch with the base distro
files.

# prerequisites

Install `systemd-container` package containing `systemd-nspawn` tool.

# conventions

 - `base` is the completely unchanged base dir
 - `env` is an overlayfs with the default packages (like gcc, git etc)
 - `session` is an overlayfs with changes from a session
 - `work` contains temp files used by eth overlayfs process
 - `overlay` is a mountpoint for the combined overlayfs image

 - "Build host": the actual build machine's OS + filesystem
 - "overlayfs": the guest filesystem

# 1) Build host: creating the sai user and sai-nspawn directory

Normal \*nix

```
$ sudo useradd sai
$ sudo mkdir -p /home/sai/sai-nspawn
```
OSX

```
$ sudo dscl . -create /Users/_sai
$ sudo dscl . -create /Users/_sai UniqueID 773
$ sudo dscl . -create /Users/_sai PrimaryGroupID -2
$ sudo dscl . -create /Users/_sai UserShell /bin/bash
$ sudo chown _sai:nobody /Users/sai
```

# 2) Build host: Creating base layers for various OSes

** Notice we're doing this on the build host, not in an overlayfs **

At least on Fedora, debootstrap is a package available from the default repos,
even though it's alien to Fedora package system itself.

## Ubuntu

```
# cd /home/sai/sai-nspawn
# mkdir linux-ubuntu-xenial-amd64
# cd linux-ubuntu-xenial-amd64
# mkdir env work
# debootstrap --arch=amd64 --variant=buildd xenial base http://archive.ubuntu.com/ubuntu/
```

Ubuntu only has amd64 and i386

## Debian

```
# cd /home/sai/sai-nspawn
# mkdir linux-debian-jessie-amd64
# cd linux-debian-jessie-amd64
# mkdir env work
# debootstrap --arch=amd64 stable base
```

Debian also has arm64 arch available.

## fedora

```
# cd /home/sai/sai-nspawn
# mkdir linux-fedora-32-x86_64
# cd linux-fedora-32-x86_64
# mkdir base env work
# dnf -y --releasever=32 --installroot=`pwd`/base install systemd passwd dnf fedora-release
```

If asked to import a key, accept it.

Fedora also has aarch64

## OSX

Install brew, take care to install `pkg-config` via brew along with gcc etc

# Overlayfs: Creating the base + env overlay to prepare env for building

Now we're going to do stuff based on mounting the overlay temporarily
rather than the initial setup, let's move to /tmp (or anywhere else
you prefer like your home dir).

When env is the active layer, we must use a work dir in the same partition.
(Later, we'll add a throwaway layer /tmp/mysession on top of base+env, then
the work dir can be in /tmp/mywork.  But when changing env the work dir
must be in the same partition.)

```
$ cd /tmp
$ sudo mkdir -p mywork myoverlay mysession
$ export MYBASE=/home/sai/sai-nspawn/linux-ubuntu-xenial-amd64
$ sudo mount -t overlay -o lowerdir=$MYBASE/base,upperdir=$MYBASE/env,workdir=$MYBASE/work none /tmp/myoverlay
```

At this point, ./myoverlay is the merged filesystem of base + the currently
empty env overlay.  Files created in ./myoverlay will not affect base, but be
isolated into ./env.  We're going to use env as a layer containing all the
build tools we will need.

Run a shell with its / in ./myoverlay...

```
$ sudo systemd-nspawn -D myoverlay
Spawning container myoverlay on /tmp/myoverlay.
Press ^] three times within 1s to kill container.
root@myoverlay:~#
```

## Overlayfs: Create sai user

Install into env the things needed for the environment as root

```
# useradd -u883 -gnogroup sai -d/home/sai -m -r
                (or -gnobody for fedora)
```

If you need to do things as root, use this to let the sai user use sudo

```
# echo "sai     ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
```

## Overlayfs: Fedora: install build env

glibc.i686 is included so you can use prebuilt 32-bit embedded toolchains, eg, linkit

```
# dnf install -y sudo make git gcc gcc-c++ cmake net-tools dhclient openssl-devel vim glibc.i686 dbus
```

Additional things for eg, building lws can be added like

```
# dnf install mbedtls-devel glib2-devel libevent-devel libev-devel libuv-devel libmount-devel sqlite-devel libgit2-devel
```


## Overlayfs: Debian / Ubuntu: install build env

Ensure vim is available

```
# apt update && apt install vim
```

Then enable "universe" repo

```
# apt edit-sources
```

and add `universe` to the end of the line after `main`

```
# apt update
```

Then install general build prerequisties, libc-i386 is included so you can use
prebuilt 32-bit embedded toolchains, eg, linkit

```
# apt install sudo make git gcc cmake net-tools libssl-dev vim libc6-i386 dbus
```
You probably want other things if you're building something with optional deps,
eg, these for various build options on lws

```
# apt install libmbedtls-dev glib2.0-dev libevent-dev libev4 libev-dev libuv1 libuv1-dev libmount-dev libsqlite3-dev libgit2-dev 
```

### Overlayfs: Finishing with layer modifications

Type `exit` to exit the overlay shell

## Build host: Unmount the overlayfs

Having left the chrooted shell, you can unmount the overlay

```
# umount myoverlay
```

The changes we make like the package installs were all captured into env

## Build host: optionally Remount so the overlay is base + env + /tmp/mysession

```
# mount -t overlay -o "lowerdir=$MYBASE/env:$MYBASE/base,upperdir=mysession,workdir=mywork" none myoverlay
```

Enter a shell again with changes going into the throwaway overlay "session" and this time as `sai` user.
From the changes we made earlier in env layer, sai has nopassword sudo access.

```
# systemd-nspawn -u sai -D myoverlay
```

From there, you can git clone your CI projects and configure, make and make install them


# Uploading nspawn layers to git

```
$ cd linux-ubuntu-xenial-amd64+base
$ sudo git init
$ sudo git add *
$ sudo git commit -m "initial commit"
$ sudo git remote add origin ssh://git@example.com/linux-ubuntu-xenial-amd64+base
$ sudo git push origin +master:master
```


