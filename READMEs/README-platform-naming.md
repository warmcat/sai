# Sai platform tuples

Platform names in sai capture the OS, arch and toolchain details in a string in
a way that allows more or less specificity for matching.

The general format is "os/arch[/toolchain]", eg, "linux-fedora/x86_64/gcc".  The
elements may contain increasingly specific details by -, eg,
"linux-fedora-32/x86_64-amd", or "freertos-espidf/xl6-esp32"

Builders should describe their platform in full detail, applications that want
to use the platforms to perform tests should only provide the detail level they
care about.

Instead of direct string matching between what the builders offer and the
project wants to be built on, the builders are matched from the left for both
the platform and architecture individually.  So, a project wanting
"linux-fedora/x86_64" will successfully match on a builder offering
"linux-fedora-32/x86_64-amd".  But a project requesting "linux-fedora-31/x86_64"
will not match it.

**NOTE:** dots may not be used in platform tuple strings

## OS part

The OS class name should be first and is mandatory, eg, "linux" or "freertos".
The OS name is next, eg, "linux-fedora" or "freertos-espidf".

Builders should go on to give more details, eg, versioning if that is relevant
for the platform, eg, "fedora-32".

Projects using `.sai.json` to specify tests may choose how much specificity to
provide, eg, just "linux-fedora" to run on whatever fedora version the builders
provide.

### Well-known OS strings

OS|meaning
---|---
linux-fedora|
linux-ubuntu|
linux-debian|
linux-gentoo|
linux-centos|
linux-android|
freertos-espidf|FreeRTOS-based espressif sdk
freertos-linkit|FreeRTOS-based Mediatek linkit SDK
netbsd-OSX|
netbsd-iOS|
windows|


## Architecture part

Similarly builders should specify at least both the arch and the vendor name of
the architecture, eg, "x86_64-amd", or "x86_64-intel".  Projects that don't care
can just specify "x86_64", ones that do care can control what it can build on
more specifically, eg, "x86_64-amd".

### Well-known architecture strings

arch|meaning
---|---
`arm32`|32-bit arm, -m0, -m0+, -m3, -m4, -a9 etc may follow
`arm32-m4-mt7697`|Mediatek linkit chip
`aarch64`|any 64-bit arm
`aarch64-a72-bcm2711`|Raspberry Pi 4 CPU
`aarch64-a72-bcm2711-rpi4`|Raspberry Pi 4 CPU in an actual RPi4
`x86_64`|Intel 64-bit (aka amd64)
`x86_64-amd`|Intel 64-bit implemented in an AMD chip
`x86_64-intel`|Intel 64-bit implemented in an Intel chip
`xl6`|Xtensa xl6
`xl6-esp32`|Xtensa xl6 implemented in an ESP32
`riscv32`|32-bit RiscV
`riscv64`|64-bit RiscV

## Toolchain part

On many platforms there is a choice of toolchain between, eg, gcc and llvm now,
and sometimes others like Intel compiler, mingw, etc.  For that reason it has to
be treated as another "dimension" in the platform tuple.

### Well-known toolchain strings

toolchain|meaning
---|---
`gcc`|OS gcc version
`llvm`|OS llvm version
`msvc`|Visual Studio toolchain
`mingw32`|Mingw compiler 32-bit
`mingw64`|Mingw compiler 64-bit

`-cross` may be appended indicating it's a cross-build rather than targeted at
the same host.

## Examples

Platform|Description
---|---
`linux-fedora/x86_64`|matches any version of fedora on x86_64 the builders provide
`linux-fedora/x86_64-amd`|as above but only on an amd builder
`linux-fedora-32`|f32 on any arch
`freertos/arm32`|any freertos-based RTOS on any arm32
`freertos-espidf/xl6-esp32/gcc-cross`|the usual esp-idf based esp32 project
`freertos-arduino/xl6-esp32/gcc-cross`|arduino OS on esp32
`linux-ubuntu-2004/aarch64-a72-bcm2711-rpi4/gcc`|Raspberry Pi 4

## Sai builder conf host names

The `"host":` member of the builder configuration also has structure.
It indicates build host platform details and the location and virtualization
situation of the `sai-builder` instance using that conf.

Its format is `"<plat>[,<plat>]"`; the platform specification follows the
platform description above, except the toolchain part is optionally replaced by
the hostname.

Eg, `"linux-fedora-32/riscv-qemu,linux-fedora/x86_64-amd/noi"`.

### Indicating virtualization

The host part of the string can indicate its virtualization by adding
a well-known suffix to the arch part, ie, indicating that the "implementor" of
the arch "device" is qemu.

Well-known virt name|Meaning
---|---
-nspawn|systemd-nspawn hosted by the string to the right
-qemu|qemu instance hosted by the string to the right

Eg, `"windows-10/x86_64-amd-qemu, linux-fedora/x86_64-amd/noi"`
