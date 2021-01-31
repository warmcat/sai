# Setting up and administering systemd-nspawn containers

Sai's approach is to run `sai-builder` from inside the environment it will do
builds from.  It reaches out to the sai-server instance and git repos, and
otherwise there is no ingress into the container or VM from outside.

So with `systemd-nspawn`, it means the containers should start on the host at
boot and run `sai-builder` inside eg from a systemd service file.  You have to:

 - one-time create the containers you want to build in
 - set them up with a `sai` user and minimal packageset
 - build libwebsockets / sai in there
 - configure `sai-builder` to run from systemd when the container starts

Systemd only runs on Linux, so this document is about how to do all those steps
for Linux hosts and Linux guests.  For Windows and OSX, the approach is similar
but using an actual machine or qemu / kvm VM.

## Create the container on the host

This will create, furnish and enter a container for Ubuntu Xenial (2016-era
long-term stable).  Other distros are basically the same with minor changes

![surprised](../assets/surprised.png)Restrictions!  Systemd with create a veth device based off your machine name
chosen here, it's limited to 11 chars.  So two machines here must not have the
same first 11 chars in their name!

![surprised](../assets/surprised.png)Restrictions!  Systemd won't let your machine name have an underscore, so no
`...x86_64`.  It's OK with `-`.

Pick your machine name, in this example `xenial-amd64`.

### Step 1: Create the initial host config for the machine

On the host, `mkdir /etc/systemd/nspawn` and then create in
`/etc/systemd/nspawn/xenial-amd64.nspawn`

```
[Exec]
Boot=no
[Network]
VirtualEthernet=no
```

We will come back and set both of those to `yes` after we have set the container
up manually from the inside.

### Step 2: Create the machine

```
Host # cd /var/lib/machines
Host # debootstrap --arch=amd64 --variant=buildd xenial xenial-amd64 http://archive.ubuntu.com/ubuntu/
Host # systemd-nspawn -M xenial-amd64
Spawning container xenial-amd64 on /var/lib/machines/xenial-amd64.
Press ^] three times within 1s to kill container.
root@myhost:~#
```

ah that's pretty nice so far, right!  We have a shell up inside the container
already.

#### Variations: Fedora Container

Replace the debootstrap part above with

```
Host # dnf -y --releasever=32 --installroot=`pwd`/fedora32-x8664 install systemd passwd dnf fedora-release
```

#### Variations: Debian Container

Replace the debootstrap part above with

```
Host # debootstrap --arch=amd64 stable buster-amd64
```

#### Variations: Centos 8

Create a file "centos8.repo"

```
[centos8-base]
name=CentOS-8-Base
baseurl=http://mirror.centos.org/centos-8/8.1.1911/BaseOS/x86_64/os
gpgcheck=1
```

... then...

```
$ dnf -c centos8.repo --releasever=8 --disablerepo=* --enablerepo=centos8-base --installroot=/var/lib/machines/centos8 groups install 'Minimal Install'
```

## Step 3: Install things into the container

While we're borrowing the host network, install things we want in the
container to configure and debug it.

(You might think you can install these at the bootstrap step, but I found this
can make trouble both in the right deps not being brought in and it violating
the ownership of `/usr/lib/.build-id` with real `root` instead of the fake
root of the inside of the container.)

Create the sai user and home dir

```
root@myhost:~# useradd -u883 -gnogroup sai -d/home/sai -m -r
```

For redhat type systems, use `-gnobody` instead
For BSDs, `useradd -u883 -g32766 -d/home/sai -m sai`

### Specific to Debian Buster / Sid

If a 32-bit install, leave out libc6-i386

```
Container # apt install sudo make git gcc cmake net-tools libssl-dev vim libc6-i386 dbus systemd-container libmbedtls-dev libev4 libev-dev libuv1 libuv1-dev libmount-dev libsqlite0-dev libgit2-dev inetutils-ping psmisc libgit2-27 libmbedcrypto3 libmbedtls12 libmbedx509-0 libmbedtls-dev libevent-2.1-6 libevent-dev libglib2.0-0 libsqlite3-0 libsqlite3-dev pkg-config libglib2.0-dev libglib2.0-0 libdbus-1-dev libdbus-1-3 g++
```

### Specific to Ubuntu

We have to enable the "universe" packageset to get normal things.

```
Container # apt update && apt install vim && apt edit-sources && apt update
```

add " universe" to the end of the line in the vim session it pops up... we also need to enable
updates so we can maintain the distro image, so add lines similar to this

```
deb http://archive.ubuntu.com/ubuntu focal-security main universe
deb http://archive.ubuntu.com/ubuntu focal-updates main universe
```

and save it.

### Specific to Xenial

```
Container # apt install sudo make git gcc cmake net-tools libssl-dev vim libc6-i386 dbus systemd-container libmbedtls-dev glib2.0-dev libevent-dev libev4 libev-dev libuv1 libuv1-dev libmount-dev libsqlite3-dev libgit2-dev inetutils-ping psmisc libsqlite-dev sqlite3 libsqlite3-0 libgit2-24 libgit2-dev libmbedcrypto0 libmbedtls10 libmbedx509-0 libmbedtls-dev libev4 libev-dev libevent-2.0 libevent-dev glib-2.0 pkg-config
```

### Specific to Bionic

```
Container # apt install sudo make git gcc cmake net-tools libssl-dev vim libc6-i386 dbus systemd-container libmbedtls-dev libev4 libev-dev libuv1 libuv1-dev libmount-dev libsqlite0-dev libgit2-dev inetutils-ping psmisc libgit2-26 libmbedcrypto1 libmbedtls10 libmbedx509-0 libmbedtls-dev libevent-2.1-6 libevent-dev libglib2.0-0 libsqlite3-0 libsqlite3-dev pkg-config libglib2.0-dev libglib2.0-0 libdbus-1-dev libdbus-1-3
```

### Specific to Focal

```
Container # apt install sudo make git gcc cmake net-tools libssl-dev vim libc6-i386 dbus systemd-container libmbedtls-dev libevent-2.1-7 libev4 libev-dev libuv1 libuv1-dev libmount-dev libsqlite0-dev libgit2-dev inetutils-ping psmisc libgit2-28 libmbedcrypto3 libmbedtls12 libmbedx509-0 libmbedtls-dev libev4 libev-dev libevent-2.1-7 libevent-dev libglib2.0-0 libsqlite3-0 libsqlite3-dev pkg-config libglib2.0-dev libglib2.0-0
```

### Specific to Fedora

```
Container # dnf install sudo make git gcc gcc-c++ cmake net-tools dhclient openssl-devel vim glibc.i686 dbus mbedtls-devel glib2-devel libevent-devel libev-devel libuv-devel libmount-devel sqlite-devel libgit2 libgit2-devel dbus systemd-container vim libpkgconf procps-ng
```

### Specific to Centos 7

```
Container #  dnf install sudo make git gcc gcc-c++ cmake net-tools dhclient openssl-devel vim glibc.i686 dbus glib2-devel libevent-devel libev-devel sqlite-devel libgit2 dbus systemd-container vim libpkgconf rpm-build systemd-networkd systemd-resolved
```

### Specific to Centos 8

Centos / RHEL 8 simply doesn't provide some -devel packages like mbedtls, libgit2...

```
Container #  dnf install sudo make git gcc gcc-c++ cmake net-tools dhclient openssl-devel vim glibc.i686 dbus glib2-devel libevent-devel libev-devel sqlite-devel libgit2 dbus systemd-container vim libpkgconf rpm-build
```

### Specific to Gentoo

```
Container # emerge dev-db/sqlite libgit2 gcc net-libs/mbedtls dev-libs/openssl cmake libuv libevent dev-libs/glib dev-libs/libev dbus pkgconf libcap
```

## Step 3: Configure the container private networking

Set up a hostname for the container by editing its `/etc/hostname`, ideally to
a name like `machinename-hostname`, eg, `xenial-myhost` if your host is `myhost`.

Configure the container's systemd-networkd to set up the private network for
the container we will switch to next.

In the container, create `/etc/systemd/network/50-host0.network` with the
contents

```
[Match]
Name=host0
[Network]
DHCP=no
IPForward=1
Gateway=192.168.80.1
Address=192.168.80.9/24
```

If you have multiple containers, give each one a different 192.168.xx network.
Enable systemd pieces to control setting the interface, route and dns.

You can avoid systemd-resolved if you like, `rm /etc/resolv.conf` to remove
its symlink, and then something like `echo "nameserver 1.2.3.4" > /etc/resolv.conf`
Either way at least enable `systemd-networkd`

```
Container # ln -sf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf
Container # systemctl enable systemd-networkd systemd-resolved
```

### Specific to Centos 8

Centos 8 has no systemd-networkd.  The base install instead has `nmcli`, you
can set up an ipv4.method=static solution that way using the same network
layout as above.

## Step 4: Switch over to systemd management of the container

Exit the container session.

On the host, edit `/etc/systemd/nspawn/xenial-amd64.nspawn` we created earlier
and set both `Boot` and `VirtualEthernet` to `yes` now.

Tell the host we want this container up when the host is up, and that we want
to start it now.

```
Host # systemctl enable systemd-nspawn@xenial-amd64
Host # systemctl start systemd-nspawn@xenial-amd64
```

Then, we can reopen a shell prompt in the now-running container

```
Host # machinectl -M xenial-amd64 shell
Connected to the local host. Press ^] three times within 1s to exit session.
[root@xenial-amd64 ~]# 
```

You can look around with ps if you have it or systemctl status and see that it
was started inside by systemd.

## Step 5: Configure the host-side networking

systemd will have created a matching network interface on your host named
`ve-machinename`, where machinename is your (truncated if neccessary)
machine name.

We need to explain to Network Manager how to deal with this so the container
will get networking services cleanly.

We will call the related NetworkManager connection `nm-...`

```
Host # export NMNAME=ve-xenial ; nmcli c add type ethernet autoconnect no con-name nm-$NMNAME ifname $NMNAME ;\
  nmcli c m nm-$NMNAME ipv4.method shared ; \
  nmcli c m nm-$NMNAME ipv4.addresses 192.168.80.1/24 ; \
  nmcli c m nm-$NMNAME connection.zone vm ; \
  nmcli c m nm-$NMNAME connection.autoconnect true ; \
  nmcli c down nm-$NMNAME ; nmcli c up nm-$NMNAME
```

## Step 6: Restart the container

```
Host # systemctl stop systemd-nspawn@xenial-amd64
Host # systemctl start systemd-nspawn@xenial-amd64
Host # machinectl -M xenial-amd64 shell
``` 

If you hit selinux AVCs on the start step, as I did on Fedora 32, you can get
around them with three policy additions

```
Host # echo 'AVC avc:  denied  { search } for  pid=1502 comm="systemd-machine" name="21786" dev="proc" ino=22140 scontext=system_u:system_r:systemd_machined_t:s0 tcontext=system_u:system_r:unconfined_service_t:s0 tclass=dir permissive=0' | audit2allow -M machine-search ; semodule -i machine-search.pp
Host # echo 'AVC avc:  denied  { read } for  pid=1502 comm="systemd-machine" name="cgroup" dev="proc" ino=60301 scontext=system_u:system_r:systemd_machined_t:s0 tcontext=system_u:system_r:unconfined_service_t:s0 tclass=file permissive=0' | audit2allow -M machine-read ; semodule -i machine-read.pp
Host # echo 'AVC avc:  denied  { open } for  pid=1502 comm="systemd-machine" path="/proc/21937/cgroup" dev="proc" ino=46779 scontext=system_u:system_r:systemd_machined_t:s0 tcontext=system_u:system_r:unconfined_service_t:s0 tclass=file permissive=0' | audit2allow -M machine-open ; semodule -i machine-open.pp
```

In the container, you should see `host0` has ben set up

```
Container # ifconfig host0
host0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500
        inet 192.168.80.9  netmask 255.255.255.0  broadcast 192.168.80.255
        inet6 fe80::fc27:1eff:fe53:4d93  prefixlen 64  scopeid 0x20<link>
        ether fe:27:1e:53:4d:93  txqueuelen 1000  (Ethernet)
        RX packets 1117  bytes 102933 (100.5 KiB)
        RX errors 0  dropped 0  overruns 0  frame 0
        TX packets 754  bytes 71331 (69.6 KiB)
        TX errors 0  dropped 0 overruns 0  carrier 0  collisions 0
```

Whether you can ping things depends on your host routing arrangements for its
side of the virtual ethernet.  If you pointed the container to your local
resolver you  need to enable that in the "vm" zone, along with masquerading.
You'll only need to do this for the first vm, afterwards it enough to tell nmcli
that the connection is in `vm` zone to inherit the functionality.

```
Host # firewall-cmd --permanent --new-zone vm
Host # firewall-cmd --permanent --zone vm --add-masquerade
Host # firewall-cmd --permanent --zone vm --add-port 53/udp
Host # firewall-cmd --permanent --zone vm --add-port 53/tcp
Host # systemctl restart firewalld
```

This might seem complicated but nmcli and firewalld (and systemd) have made
all this a lot easier to set up than a few years ago.

## Get, build and install lws

We're going to do it as root in the container... I produce and host this code,
you may feel its riskier in which case create a new user with sudo rights to
handle it.

For redhat type distros, you probably need to add /usr/local/lib to the
/etc/ld.so.conf before ldconfig can rgister the new libwebsockets.so

```
Container # git clone https://libwebsockets.org/repo/libwebsockets
Container # cd libwebsockets && mkdir build && cd build && \
  cmake .. -DLWS_WITH_JOSE=1 -DLWS_UNIX_SOCK=1 -DLWS_WITH_STRUCT_JSON=1 \
   -DLWS_WITH_STRUCT_SQLITE3=1 -DLWS_WITH_GENCRYPTO=1 -DLWS_WITH_SPAWN=1 \
   -DLWS_WITH_SECURE_STREAMS=1 -DLWS_WITH_THREADPOOL=1 -DLWS_WITH_JOSE=1 \
   -DLWS_WITH_PLUGINS_BUILTIN=1
Container # make -j && make -j install && ldconfig
```

For OSX, you'll need to add something like `-DLWS_OPENSSL_INCLUDE_DIRS=/usr/local/opt/openssl@1.1/include -DLWS_OPENSSL_LIBRARIES="/usr/local/opt/openssl/lib/libssl.dylib;/usr/local/opt/openssl/lib/libcrypto.dylib"` to the cmake line

For newer M1 based OSX

```
cmake .. -DLWS_OPENSSL_INCLUDE_DIRS=/opt/homebrew/Cellar/openssl@1.1/1.1.1h/include '-DLWS_OPENSSL_LIBRARIES=/opt/homebrew/Cellar/openssl@1.1/1.1.1h/lib/libssl.dylib;/opt/homebrew/Cellar/openssl@1.1/1.1.1h/lib/libcrypto.dylib' -DLWS_WITH_MINIMAL_EXAMPLES=0 -DLWS_WITH_JOSE=1 -DLWS_UNIX_SOCK=1 -DLWS_WITH_STRUCT_JSON=1 \
    -DLWS_WITH_GENCRYPTO=1 -DLWS_WITH_SPAWN=1 \
   -DLWS_WITH_SECURE_STREAMS=1 -DLWS_WITH_THREADPOOL=1 -DLWS_WITH_JOSE=1 \
   -DLWS_WITH_PLUGINS_BUILTIN=1
```

### Windows: build libgit2

For Windows, after following the way to build lws in
`READMEs/README.build-windows.md` in the lws tree, you need to build libgit2

```
$ git clone https://github.com/libgit2/libgit2
$ cd libgit2 && mkdir build && cd build && cmake .. -DBUILD_CLAR=OFF
$ cmake --build . --config DEBUG
```

Then, in and Administrator-privs cmd window

```
$ cmake --install . --config DEBUG
```

### Get, build and install sai

#### Linux

```
Container # cd ~
Container # git clone https://warmcat.com/repo/sai
Container # cd sai && mkdir build && cd build && cmake .. && make && make install
Container # cp ../scripts/sai-builder.service /etc/systemd/system
Container # mkdir -p /etc/sai/builder
Container # cp ../scripts/builder-conf /etc/sai/builder/conf
Container # vim /etc/sai/builder/conf
Container # systemctl enable sai-builder
```

#### Windows 

```
$ git clone https://warmcat.com/repo/sai
$ cd sai && mkdir build && cd build
$ cmake .. -DSAI_SERVER=0 -DSAI_LWS_INC_PATH="C:/Program Files (x86)/libwebsockets/include" -DSAI_LWS_LIB_PATH="C:/Program Files (x86)/libwebsockets/lib/websockets.lib"  -DSAI_EXT_PTHREAD_INCLUDE_DIR="C:\Program Files (x86)\pthreads\include" -DSAI_EXT_PTHREAD_LIBRARIES="C:\Program Files (x86)\pthreads\lib\x64\libpthreadGC2.a" -DLWS_OPENSSL_INCLUDE_DIRS="C:\Program Files\OpenSSL\include" -DLWS_OPENSSL_LIBRARIES="C:\Program Files\OpenSSL\lib\libssl.lib;C:\Program Files\OpenSSL\lib\libcrypto.lib" -DSAI_GIT2_LIB_PATH="C:\Program Files (x86)\libgit2\lib\git2.lib" -DSAI_GIT2_INC_PATH="C:\Program Files (x86)\libgit2\include"
$ cmake --build . --config DEBUG
```

Hack the libgit2 and lws dlls into visibility (or do it a better non hacky
way if you know how...) using an Administrator cmd window

```
# cp "C:\Program Files (x86)\libgit2\bin\git2.dll" C:\Windows\system32
# cp "C:\Program Files (x86)\libwebsockets\bin\websockets.dll"  C:\Windows\system32 
```

Also in the Administrator window, copy and edit the sai-builder config

```
# mkdir -p C:\ProgramData\sai\builder\
# cp ../scripts/builder-conf C:\ProgramData\sai\builder\conf
# vim C:\ProgramData\sai\builder\conf
```

#### OSX

For OSX, you'll need to adapt the cmake to something like

```
cmake .. -DLWS_OPENSSL_INCLUDE_DIRS=/usr/local/opt/openssl@1.1/include -DLWS_OPENSSL_LIBRARIES="/usr/local/opt/openssl/lib/libssl.dylib;/usr/local/opt/openssl/lib/libcrypto.dylib"
```

On the newer M1 OSX, honebrew has to be teleported in /opt, the following worked

```
cmake .. -DSAI_SERVER=0 -DSAI_GIT2_INC_PATH=/opt/homebrew/Cellar/libgit2/1.1.0/include -DSAI_GIT2_LIB_PATH=/opt/homebrew/Cellar/libgit2/1.1.0/lib/libgit2.dylib -DLWS_OPENSSL_INCLUDE_DIRS=/opt/homebrew/Cellar/openssl@1.1/1.1.1h/include '-DLWS_OPENSSL_LIBRARIES=/opt/homebrew/Cellar/openssl@1.1/1.1.1h/lib/libssl.dylib;/opt/homebrew/Cellar/openssl@1.1/1.1.1h/lib/libcrypto.dylib'
```

Make sai-builder run as a daemon, create the file `/Library/LaunchDaemons/com.warmcat.sai-builder.plist`

```
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN"
    "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.warmcat.sai-builder</string>
    <key>ServiceDescription</key>
    <string>Sai Builder Daemon</string>
    <key>ProgramArguments</key>
    <array>             
        <string>/usr/local/bin/sai-builder</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>StandardErrorPath</key>
    <string>/var/log/sai-builder.log</string>
</dict>
</plist>
```

```
% sudo launchctl load /Library/LaunchDaemons/com.warmcat.sai-builder.plist
```

That's it, he should be running in the background.  To stop him

```
% sudo launchctl stop com.warmcat.sai-builder
```

### Make the container start at boot

Let's leave the container and go back to the host

```
Container # exit
Host #
```

Then, we can configure it to start on boot

```
Host # systemctl enable systemd-nspawn@xenial-amd64
```

## Administration of containers

The container should be up and running sai-builder, and should come back up
subsequent boots too.  You can use machinectl to open a shell on the running
container like this:

```
Host # machinectl -M xenial-amd64 shell
Connected to the local host. Press ^] three times within 1s to exit session.
Container # ps fax
    PID TTY      STAT   TIME COMMAND
      1 ?        Ss     0:00 /lib/systemd/systemd
     14 ?        Ss     0:00 /lib/systemd/systemd-journald
     22 ?        Ss     0:00 /lib/systemd/systemd-logind
     23 ?        Ss     0:00 /usr/bin/dbus-daemon --system --address=systemd: --nofork --nopidfile --systemd-activation
     25 ?        Ssl    0:00 /usr/local/bin/sai-builder
     28 pts/0    Ss+    0:00 /sbin/agetty --noclear --keep-baud console 115200 38400 9600 vt220
     42 ?        Ss     0:00 /lib/systemd/systemd-machined
     43 pts/1    Ss     0:00 -bin/sh
     52 pts/1    S      0:00  \_ bash
     77 pts/1    R+     0:00      \_ ps fax
Container #
```

You can use journalctl etc in there to see what's happening with sai-builder
there, etc.

## Distro age considerations

Really old distros are not maintained any more for updates, and their tls
certs have expired so they can't do https... it's pretty hopeless.

Xenial (16.04) has problems with cmake (not all CTest functionality) and
libgit2 (remote fetch broken on its 0.24 version).  You can rapidly build
both of these by cloning, they have CMake-based flow that's easy to use.

Package|upstream
---|---
cmake|https://github.com/Kitware/CMake.git
libgit2|https://github.com/libgit2/libgit2.git

Install into the default /usr/local, and remove the conflicting old distro
packages.

## Other Linux distros

Other modern Linux distros with systemd can be done in their own containers and
also made to start at boot... these are very lightweight compared to full VMs
and yet you can administer them using all the usual systemd goodies including
randomly opening shells into their universe if needed.

# Flashing and testing on embedded devices

## Exposing host assets in the containers

For the embedded case you want to flash and test on an external board, you need
to expose typically USB tty devices inside the nspawn container safely.

That's not so trivial or as useful as it could be and has some limitations.

### Step 1: Bind the related devices

Device binding inside the container is like a one-time thing at container
start, wildcards are not supported and if the device doesn't exist, the
container won't start.  All this makes supporting ephemeral USB serial ports
that come and go impossible.

If your USB tty device is always available, then you can elect to expose it to
the container by overriding the systemd service definition for your container,
eg

```
Host # mkdir /etc/systemd/system/systemd-nspawn@YOURMACHINE.service.d
Host # vim /etc/systemd/system/systemd-nspawn@YOURMACHINE.service.d/override.conf
```

and add in there something like

```
[Service]
DeviceAllow=char-ttyUSB rwm
DeviceAllow=char-usb_device rwm
```

### Step 2: loosen the permissions on the device at the host

I wasn't able to find a way to let the container unprivileged user seem to join
the host `dialout` group.  The only workaround was to allow general access to
the device node.

Create a file `vim /etc/udev/rules.d/50-allow-nspawn-ttyUSB.rules` and add in
there

```
KERNEL=="ttyUSB[0-9]*",MODE="0666"
KERNEL=="ttyACM[0-9]*",MODE="0666"
```

### Step 3: Bind the device nodes into the container

Edit the container's nspawn file, like `/etc/systemd/nspawn/MYMACHINE.nspawn`
and in the `[Files]` section add

```
Bind=/dev/serial
Bind=/dev/ttyUSB0
```

This is very awkward because the device nodes like `/dev/ttyUSB0` must exist at
container start time or it will fail to start, and you can't expose them via the
`/dev/serial/by-id` symlinks either, or use wildcards since it's done as a
one-time init at container start.
