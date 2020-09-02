# Building Sai Builder for windows

The overall flow is

 - build lws
 - build libgit2
 - build sai-builder piggybacking on pthreads built for lws

## Building lws

Refer to [the instructions in lws](https://libwebsockets.org/git/libwebsockets/tree/READMEs/README.build-windows.md)
for how to install `cmake`, `git` and the toolchain on windows
also used in the next steps here.

For windows + sai-builder, the following lws cmake config is suitable

```
> cmake .. -DLWS_WITH_JOSE=1 -DLWS_HAVE_PTHREAD_H=1 -DLWS_EXT_PTHREAD_INCLUDE_DIR="C:\Program Files (x86)\pthreads\include" -DLWS_EXT_PTHREAD_LIBRARIES="C:\Program Files (x86)\pthreads\lib\x64\libpthreadGC2.a" -DLWS_WITH_MINIMAL_EXAMPLES=1 -DLWS_WITH_THREADPOOL=1 -DLWS_UNIX_SOCK=1 -DLWS_WITH_STRUCT_JSON=1 -DLWS_WITH_SPAWN=1 -DLWS_WITH_SECURE_STREAMS=1 -DLWS_WITH_DIR=1  
```

## Building libgit2

```
> git clone https://github.com/libgit2/libgit2.git
> cd libgit2 ; mkdir build ; cd build
> cmake .. -DBUILD_CLAR=OFF
> cmake --build . --config DEBUG
```

I couldn't get runas to do anything useful, I opened an Admin-privileged
cmd window and did

```
> cd \Users\agreen\libgit2\build
> cmake --install . --config DEBUG
> exit
```

## Building Sai-builder

```
> git clone https://warmcat.com/repo/sai
> cd sai ; mkdir build ; cd build
> cmake .. -DSAI_MASTER=0 -DSAI_LWS_INC_PATH="\Users\agreen\libwebsockets\build\include" -DSAI_LWS_LIB_PATH="\Users\agreen\libwebsockets\build\lib\Debug\websockets_static.lib" -DLWS_OPENSSL_INCLUDE_DIRS="\Program Files\OpenSSL\include" -DSAI_GIT2_INC_PATH="\Program Files (x86)\libgit2\include" -DSAI_GIT2_LIB_PATH="\Program Files (x86)\libgit2\lib\git2.lib" -DSAI_EXT_PTHREAD_INCLUDE_DIR="C:\Program Files (x86)\pthreads\include" -DLWS_OPENSSL_LIBRARIES="\Program Files\OpenSSL\lib\libcrypto.lib;\Program Files\OpenSSL\lib\libssl.lib"  -DSAI_EXT_PTHREAD_LIBRARIES="C:\Program Files (x86)\pthreads\lib\x64\libpthreadGC2.a" 
```

## Configuring Sai-builder

```
>net user sai /add
```

As administrator

```
> mkdir \ProgramData\sai\builder
```

Then create the config JSON in `\ProgramData\sai\builder\conf`, using the
platform name `windows-10`.
