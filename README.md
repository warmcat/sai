![Sai](./assets/sai.svg)

`Sai` (pronouced like 'sigh', "Trial" in Japanese) is a very lightweight
network-aware remote CI runner.  It knows how to use `systemd-nspawn` and
`overlayfs` to build remote git repos on a variety of distro versions very
cheaply.

On small targets, it can also build on the host rootfs directly, and can also
run on an RPi3-class 'buddy' to flash and collect results (eg, over serial, or
gpio) from even smaller targets.

A self-assembling constellation of Sai Builders make their own connections to a
master, who then received hook notifications, distributes work concurrently over
idle builders that have the required environment, and logs results.

## General approach

 - Sai builders for various platforms and scenarios are configured and run
   independently... they maintain "nailed-up" connections to the master over
   wss, and announce their capabilities.  The builders do not need any listening
   ports or other central management this way.

 - When a git repo being monitored is updated, a hook performs a GET notiftying
   the Sai master.

 - The Sai master fetches `.sai.json` from the repo and queues jobs on connected
   builders that have the platform and OS versions requested for build in
   the `.sai.json`.

 - On the builders, result channels (like stdout, stderr) with logs and
   results are first listed and then streamed back to the master and the build
   proceeds.  Discrete files may also be streamed to the master like this.

 - The master makes human readable current and historical results available
   in realtime over https

 - The Sai master node also has a builder integrated.  So to get started you
   can do it all on one machine.

## Creating a selfsigned TLS cert

You can create a P-521 EC key and self-signed certificate for use with Sai
master like this:

```
$ openssl ecparam -name secp521r1 -genkey -param_enc explicit -out sai-selfsigned-key.pem
$ echo -e "\n\n\n\n\n\n" | openssl req -new -x509 -key sai-selfsigned-key.pem -out sai-selfsigned-cert.pem
```

There's a helper script `sudo ./scripts/gen-selfsigned-certs.sh` to do this and
install the results in `/usr/local/share/sai`, you can override the install
directory by giving it as a commandline argument to the script.

Or alternatively you can use your own PEM certificate and key from a trusted
certifcate vendor, which matches your domain name.

Self-signed is secure, but your browser will make you explicitly trust it before
it will let you use it.  Afterwards, your browser will notice if the certificate
is different and untrusted for that site and warn you.  Because you must
generate it after install, your selfsigned cert will be unique.

