# sai-power

## Overview

sai-power is an optional helper daemon that is designed to run on the same
subnet as the builders, and be up all the time.

It has two tasks:

 - monitor the situation on sai-server on behalf of that are OFF or suspended,
   and turn them on / resume them when there are jobs they could run on the
   platforms they offer.

 - help builders who see they are idle and want to shut down, by powering them
   OFF via their smartplug a short time after requested to do so.

## Configuration of sai-power

sai-power looks for JSON config at /etc/sai/power/conf.  It looks like this

```
{
	"perms":	"sai:nobody",

	"servers":	[
		{
			"url" : "wss://libwebsockets.org:4444/sai/builder",

			"platforms": [
				{
					"name": "rocky9/x86_64-amd/gcc",
					"host": "l2",
					"power-on":	{
						"type": "wol",
						"mac": "00:e0:4c:68:01:30"
					},
					"power-off":	{
						"type": "suspend"
					}
				},
				{
					"name": "rocky9/aarch64-a72a55-rk3588/gcc",
					"host":	"b32",
					"power-on":     {
						"type": "tasmota",
						"url": "http://10.199.0.240/cm?cmnd=Power%20On"
					},
					"power-off":    {
						"type": "tasmota",
						"url": "http://10.199.0.240/cm?cmnd=Power%20Off"
					}
				},
				{
					"name": "ubuntu-noble/riscv64/gcc",
					"host":	"rv2",
					"power-on":     {
						"type": "tasmota",
						"url": "http://10.199.0.104/cm?cmnd=Power%20On"
					},
					"power-off":    {
						"type": "tasmota",
						"url": "http://10.199.0.104/cm?cmnd=Power%20Off"
					}
				}

			]
		}
	]
}
```

It describes one or more sai-server that it should keep tabs on, and for each server,
a list of builders and how to manage their power.

Two types of builder power management is supported

 - "suspend"... the builder will enter suspend mode by itself after it sees that it
   has been idle (there are no more jobs) for some time.  sai-power will resume it
   when jobs that it can handle appear, using WOL.

 - "tasmota"... the builder will ask sai-power to turn off its smartplug in a few
   seconds, then enter shutdown.  sai-power will turn it back on again at the
   smartplug when jobs it can handle appear at a server it knows how to use.

## Configuration at the builder

Builders that participate in sai-power management need to point to sai-power
in ther configuration, for example

```
        "host":         "myhostname",
	"sai-power":    "http://10.199.0.10:3333",
```

The builder's "host" entry is matched to the sai-power configuration to allow
the builder to request sai power to turn itself off.

## Builder power control manually

Builders that are managed by sai-power are OFF a lot, basically any time it finished
building the last push until the next one, which may be whole days or more.

If you need to ssh in to them, that would be inconvenient since they are literally OFF.

To simplify that case, you can manually ask sai-power to start up a builder and
for that session only, disable its idle detection.

```
# wget -O- http://10.199.0.10:3333/power-on/hostname
```

Asking sai-power to do it has some advantages:

 - it will work the same no matter the details of that particular builder's
   power arrangements, ie, if suspend / resume or needs a specific smartplug,
   sai-power knows what to do depending on the hostname

 - sai-power can remember if you did it manually (as opposed to sai-power
   starting the builder since it saw jobs available) and inform the builder
   after it starts that for this session, it shouldn't auto suspend /
   power down.


