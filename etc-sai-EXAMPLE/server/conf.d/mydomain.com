# you should create one file like this per vhost in /etc/sai/server/conf.d

{
	"vhosts": [{
		"name": "mydomain.com",
		"port": "4444",
		"host-ssl-key":  "/etc/letsencrypt/live/libwebsockets.org/privkey.pem",
		"host-ssl-cert": "/etc/letsencrypt/live/libwebsockets.org/fullchain.pem",
		"access-log": "/var/log/sai-server-access-log",
		"disable-no-protocol-ws-upgrades": "on",
		"enable-client-ssl": "on",
		"allow-non-tls": "1",

		"mounts": [
			{
				"mountpoint":		"/sai",
				"default":		"index.html",
				"origin":		"file:///usr/local/share/sai/assets",
				"origin":		"callback://com-warmcat-sai",
				"cache-max-age": 	"7200",
				"cache-reuse":		"1",
				"cache-revalidate":	"0",
				"cache-intermediaries":	"0",
	 			"extra-mimetypes": {
					".zip": "application/zip",
					".map": "application/json",
					".ttf": "application/x-font-ttf"
				}
			},
			{
				# semi-static cached avatar icons
				"mountpoint": "/sai/avatar",
				"origin": "callback://avatar-proxy",
				"cache-max-age": "2400",
				"cache-reuse": "1",
				"cache-revalidate": "0",
				"cache-intermediaries": "0"
			}
		],
		
		#
		# these headers, which will be sent on every transaction, make
		# recent browsers definitively ban any external script sources,
		# no matter what might manage to get injected later in the
		# page.  It's a last line of defence against any successful XSS.
		#
		
		"headers": [{
		        "content-security-policy": "default-src 'none'; img-src 'self' data:; script-src 'self'; font-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none';",
		        "x-content-type-options": "nosniff",
		        "x-xss-protection": "1; mode=block",
		        "referrer-policy": "no-referrer"
		}],

		"ws-protocols": [
		{"com-warmcat-sai": {
			# this is the lhs of the path to use, not a full filepath
			# the events go in ...-events.sqlite3 and the related
			# info for an event goes in its own database file in
			# ...-event-xxxx.sqlite3 where xxxx is the event uuid.
			#
			"database":		"/srv/sai/sai-master",
                        #
                        # the push hook notification script creates a JSON
                        # object and signs it with a secret... "notification-
                        # key" should match the secret the notification hook
                        # script has that we're willing to accept.
                        #
                        # It should be a 64-char hex-ascii representation of
                        # a random 32-byte sequence, you can produce a strong
                        # random key in the correct format like this
                        #
                        # dd if=/dev/random bs=32 count=1 | sha256sum | cut -d' ' -f1
                        #
			"notification-key":	"51b3ee2f06ef2a893cfe901972bd13065d7dbae4cf087b396ee38e7bf78f79a6",

			# auth jwk path
			# You can generate a suitable key like this
			#
			# lws-crypto-jwk -t EC -b512 -vP-521 --alg ES512 > mykey.jwk
			#
			"jwt-auth-alg":		"ES512",
			"jwt-auth-jwk-path":	"/etc/sai/web/auth.jwk",

			"jwt-iss":		"com.warmcat",
			"jwt-aud":		"https://mydomain.com/sai",

			# template HTML to use for this vhost.  You'd normally
			# copy this to gitohashi-vhostname.html and modify it
			# to show the content, logos, links, fonts, css etc for
			# your vhost.  It's not served directly but read from
			# the filesystem.  Although it's cached in memory by
			# gitohashi, it checks for changes and reloads if
			# changed automatically.
			#
			# Putting logos etc as svg in css is highly recommended,
			# like fonts these can be transferred once with a loose
			# caching policy.  So in practice they cost very little,
			# and allow the browser to compose the page without
			# delay.
			#
			"html-file":	 "/usr/local/share/gitohashi/templates/gitohashi-example.html",
			#
			# vpath required at start of links into this vhost's
			# gitohashi content for example if an external http
			# server is proxying us, and has been told to direct
			# URLs starting "/git" to us, this should be set to
			# "/git/" so URLs we generate referring to our own pages
			# can work.
			#
			"vpath":	 "/sais/",
			#
			# url mountpoint for the avatar cache
			#
			"avatar-url":	 "/sais/avatar/",
			#
			# libjsgit2 JSON cache... this
			# should not be directly served
			#
			"cache-base":	"/var/cache/libjsongit2",
			#
			# restrict the JSON cache size
			# to 2GB
			#
			"cache-size":   "2000000000",
			#
			# optional flags, b0 = 1 = blog mode
			#
			"flags": 0
			#"blog-repo-name":	"myrepo"
		},
		"avatar-proxy": {
			"remote-base": "https://www.gravatar.com/avatar/",
			#
			# this dir is served via avatar-proxy
			#
			"cache-dir": "/var/cache/libjsongit2"
		}
		}
		]
		}
	]
}
