# Building Sai on BSDs

This is what I found as an experieced linux user when setting
up lws + sai on NetBSD for arch64 Big Endian (`aarch64_be`) from 
a minimal image.

https://mail-index.netbsd.org/port-arm/2020/12/03/msg007117.html

## Packaging

NetBSD has a system 'pkgsrc' that is equivlent to package source
and binary builds.  Normally, you can install binary packages from
a central repository for your arch, as you would expect.  This
looks like `pkg_add mypackage`.

https://cdn.netbsd.org/pub/pkgsrc/packages/NetBSD/

But arch64BE is too new to have the package repo populated yet...
the base install comes with basic build tools like cvs, gcc and make,
everything else has to be built from the source package, which
is a day of build time on an RPi3.  However it's doable and there
and enough pieces there.

Pkgsrc is not a simple build wrapper, it understands recursive package
deps and can sub-build those automatically.  So building large projects
is not a matter of having to chase everything down and build by hand,
it knows how to do it for you for toplevel projects with many deps.

## Bootstrapping pkgsrc

### 1) Prepare pkgsrc

As root, follow the advice here for quickstart/bootstrap

https://www.pkgsrc.org/

```
cvs -danoncvs@anoncvs.netbsd.org:/cvsroot checkout pkgsrc
cd pkgsrc/bootstrap
./bootstrap
```

### 2) Build pkgsrc packges from source

Then, where x/y is the package category and name

```
# cd pkgsrc/x/y
# make install clean
```

for these packages

```
# devel/git
# devel/cmake
# devel/libgit2
# shells/bash
# security/mozilla-rootcerts
# sysutils/daemond
```

The base install has a very simplified vi, you may also want
to build `editors/vim`

### 3) symlink to executables

Some packages are referred to by well-known paths
like `/bin/bash`

```
# ln -s /usr/pkg/bin/bash /bin/bash
# ln -s /usr/pkg/bin/cmke /bin/cmake
```

### 4) Set up CA bundle abd openssl cnf

The base OS doesn't have a trusted C bundle for openssl,
additionlly install the rootcert bundle:

```
# mozilla-rootcerts install
```

The openssl it comes with expects a config file in order to
create certs, as needed by lws configuration.  Create
`/etc/openssl/openssl.cnf` with this:

```
[req]
default_bits=2048
encrypt_key=no
default_md=sha256
distinguished_name=req_subj
[req_subj]
commonName="Fully Qualified Domain Name (FQDN)"
emailAddress="Administrative Email Address"
countryName="Country Name (2 letter code)"
countryName_default=US
stateOrProvinceName="State Name (full name)"
stateOrProvinceName_default=Ohio
localityName="Locality Name (e.g., city)"
localityName_default=Columbus
organizationName="Organization Name (e.g., company)"
organizationName_default=Widgets Inc
organizationalUnitName="Organizational Unit Name (e.g., section)"
organizationalUnitName_default=Some Unit
```

After that there are enough pieces to fetch from git and
build lws and sai-builder as you would expect.

### 5) Build lws + sai

lws requires the additional cmake flag `-DLWS_WITHOUT_DAEMONIZE=0`

There is no sudo, I dealt with make install by having a root
and user console session open in parallel on the same build
dir.

### 6) Set up init

Copy `./scripts/etc-rc.d-sai_builder-NetBSD` to `/etc/rc.d/sai_builder`

Edit `/etc/rc.conf` and add `sai_builder=YES` at the bottom to start
it at boot, then `/etc/rc.d/sai_builder start`

