# Sai-jig

## Introduction

Sai-jig is a standalone daemon that normally doesn't run on the builder or
master machines, but on a smaller helper close to embedded targets and
physically wired to, eg, the reset button or flash config GPIO on them.
The helper is typically an RPi or similar machine that has networking,
Linux and convenient GPIO.

Sai-jig lets you formally describe the gpio and sequences involving it using
static JSON config on the helper machine, then opens an HTTP server on a
configured interface and port for the builders to request management of DUT
embedded boards using the sequences defined by name in the config, as part of
the CTest flow for the device.

The configuration supports multiple targets each with their own gpio namespace,
so one helper board can manage many DUTs each using their own gpio.

## Configuration example

```
{
	"schema":	"sai-jig",
	"port":		44000,
	"targets":	[
		{
			"name": "linkit-7697-1",
                	"gpios": [
                	        {
					"chip_index":	0,
					"name":		"nReset",
                                	"offset":	17,
                                	"wire":		"RST",
					"safe":		0
	                        }, {
					"name":		"usr",
	                                "chip_index":	0,
                                	"offset":	22,
                                	"wire":		"P6",
					"safe":		0
				}
                        ], "sequences": [
                        	{
					"name":		"reset",
					"seq": [
		                                { "gpio_name": "nReset", 	"value": 0 },
		                                { "gpio_name": "usr",		"value": 0 },
	        	                        {				"value": 300 },
		                                { "gpio_name": "nReset",	"value": 1 }
					]
                        	}, {
					"name":		"flash",
					"seq": [
		                                { "gpio_name": "nReset",	"value": 0 },
		                                { "gpio_name": "usr",		"value": 1 },
	        	                        {				"value": 300 },
		                                { "gpio_name": "nReset",	"value": 1 },
	        	                        {				"value": 100 },
		                                { "gpio_name": "usr",		"value": 0 }
					]
                        	}
                	]
		}
	]
}
```

JSON elements

Name|Meaning
---|---
`schema`|Must be `sai-jig`
`port`|Which port number to listen on
`iface`|Optional, which network interface to bind the listen port to
`targets`|All remaining config is specific to each target listed here
`targets/name`|The name of the target, used by remote clients
`targets/gpios`|List of gpios used by the target
`targets/gpios/name`|Name the target uses for this gpio
`targets/gpios/chip_index`|The libgpiod / linux gpiochip index, usually 0
`targets/gpios/offset`|The libgpiod gpio index inside the chip, usually "the gpio number" like 17
`targets/gpios/wire`|String documenting where the gpio is wired to on the target, not used by sai-jig
`targets/gpios/safe`|The "safe" level to init the gpio to, 0 or 1
`targets/sequences`|Describes named sequences of GPIO activity the remote client can ask to run
`targets/sequences/name`|The sequence name
`targets/sequences/seq`|A list of gpio actions done by the sequence
`targets/sequences/seq/gpio_name`|The gpio name changes as part of the sequence, indicates `value` is a time in ms if absent
`targets/sequences/seq/value`|0 or 1 to set the named gpio, or if that is absent, how many ms to pause before doing the next step

## Triggering sequences

sai-jig runs a normal HTTP server on the requested port bound to the requested
interface.  You can use normal HTTP tool like `curl` to trigger sequences on
specific targets, the HTTP request returns with a 200 when the sequence completes,
so it's easy to synchronize even though the requestor may be on a different machine.

The transaction should be an HTTP GET to `/target-name/sequence-name`, eg with the
example config above, `curl http://myhelperip:myport/linkit-7697-1/flash`.

