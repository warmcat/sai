# Sai fleet logs

## Why

The logs that explain a sai misbehaviour are usually spread over several
machines: sai-server, sai-virt / sai-power, and one or more sai-builders.  For
an ephemeral sai-virt VM, the builder's log is gone once the VM is destroyed.
Without this, finding out what happened means someone collecting journals from
each machine by hand, if they still exist.

This collects the log lines of every sai daemon into sai-server, where they can
be queried together, by an admin, or by a non-interactive client, eg, an agent
working on a sai problem.  The intended loop is: query the logs, add the
logging that turns out to be missing, deploy, rerun, query again.

## The model

There are usually several builds going on at once, and every daemon logs about
all of them interleaved, so the common basis for all logs is **UTC**.

On top of that, lines can be **tagged** with the build they are about, using
the 8-character job dir name, `xxxxxxxx`: the last 8 characters of the task
uuid (`saib_task_jobdir_vn()`).  Not the first ones: they are the event uuid,
the same for all the event's tasks.  That's per task, so all runs of a task
share it, and the run number is kept beside it.

A tag lets you turn down the noise from other builds.  It mustn't be the only
filter, though: the lines that explain a lost build are often not about any
build, eg, a builder going into power-off, sai-virt destroying its VM, the
server logging the link closing (`[DISCON]`).  So queries offer three views:

View|What's in it
---|---
`window`|Every line from every source in the time window
`focused`|In the window: lines tagged with the build, untagged lines from the hosts involved in it (the builders that ran any run of it, sai-server, sai-virt / sai-power), and every warning or error from anywhere
`hash`|Only lines tagged with the build

For a build, the window defaults to its first run's start to its last run's
end (or now, if it's still going), padded by 60s each side.

## How it's done

This uses lws's log shipping, see lws' READMEs/README.log-ship.md: every
daemon ships its lines with an lws log shipper, and sai-server keeps them, its
own too, with lws's `lws-log-ingest` plugin on its vhost, in a db of their
own beside the events db.  sai-web reads that db for the queries below, like
everything else it shows from sai-server's dbs.

The ingestor's `always` keeps sai's daemons always shipping, so, unlike a DHT
debugging session, there's nothing to start: every line goes to sai-server as
it's logged.  The ingestor's own api and viewer can be used on the same db,
eg, to raise the daemons' log level for a while with a session, but sai
doesn't need them.

## Sources

sai-server, sai-builder, sai-power and sai-virt each ship the lws log lines
they emit.  sai-web isn't a source, its logs don't bear on the problems this is
for.  A source is identified by

 - `kind`: `sai-server`, `sai-builder`, `sai-power` or `sai-virt`
 - `host`: the host name, or for a builder in a sai-virt VM, its VM name
 - `inst`: 16 random hex characters chosen when the daemon starts

`inst` matters because sai-virt reuses VM names: `sai-vm-...-2` today is not
the same machine as `sai-vm-...-2` an hour ago, and a restarted daemon starts
its sequence numbers again.

Each daemon logs what its system looks like when it starts, a line per item
prefixed `sysinfo:`: the lws build, hostname, OS, cpus and RAM, the time,
every address on every interface, and the resolver's nameservers and search
list (`lws_sysinfo`, see lws' README.sysinfo.md).  So the fleet logs say
where each source's lines came from, and for a sai-virt VM that can't reach
anything, what addresses and resolver it came up with.

A source's `host` can change.  A builder keeps what it logs from when its lws
context is made, and ships it once it's operational, that is, once its clock
is right and it has an address, but before sai-virt has told a VM which VM it
is: so what goes wrong before then, eg, a sai-virt VM that can't reach
sai-virt or resolve its name, is here too.  Until then it ships under the host
name in its conf, and takes the VM's name once sai-virt has told it.  The
lines it shipped before are the same daemon's, so the ingestor renames the
source, and they are found under the VM's name too.

The lines kept are the ones the daemon logs at its configured level (`-d`);
raising a daemon's level is how to get more detail into the fleet logs.  What
isn't an lws log line, like coredumps, OOM kills, systemd restarts, isn't here
yet.

Each line is kept with its UTC time to the us, its level, and a sequence
number per source.  lws's local time and level prefix is taken off, and
anything not printable ASCII is replaced with `?`, so whatever was logged is
harmless in JSON and in a browser.  Lines are cut at 1024 chars.  A daemon
keeps up to 4MiB of lines sai-server hasn't stored yet; past that the oldest
go, and a line says how many.  They're in memory, so lines not yet stored are
lost if the daemon dies, but not if its connection to sai-server does.

### Tagging

As each line is sent, sai's tagger, `saicom_fleetlog_tagger()`, tags it with
the build it's about, its job dir name, `xxxxxxxx`: the last 8 characters of
the task uuid (`saib_task_jobdir_vn()`).  A line is about a
build if it has `[b:xxxxxxxx]` or `[b:xxxxxxxx/rN]` in it, eg

```
lwsl_notice("[b:%s/r%d] %s: allocated to %s\n", uuid + SAI_BUILD_HASH_OFS, run,
	    __func__, builder);
```

which tags it `xxxxxxxx/rN`, or if it mentions a task uuid, 64 hex of either
case, which tags it `xxxxxxxx`.  sai-server already logs task uuids in many
places, so this tags a lot without any logging changing.  The hash is kept in
lowercase.

The ingestor keeps when each tag was first and last seen, which is a build's
window when only its job dir name is known; and the lines tagged because they
mention its uuid say which task it is.

## Shipping

Each daemon connects to the first sai-server in its conf, at its link's url
with the last path part changed to `logship`, eg
`wss://sai.example.com:4444/sai/logship`, with ws subprotocol
`lws-log-ingest`; the path doesn't matter to sai-server.  The link key is the
secret the shippers and the ingestor share.  sai-server's own lines are stored
directly.

See lws' README.log-ship.md for what's said on the connection, and what the
ingestor checks of it: whatever a source sends is checked as if it came from
anywhere, and each source may store 12000 lines a minute.

## Storage at sai-server

The ingestor's db, by default `<database>-logs.sqlite3`, eg
`/srv/sai/sai-master-logs.sqlite3`, in WAL mode so sai-web can read it while
sai-server writes.  Its tables are lws-log-ingest's, see lws'
README.log-ship.md: `sources`, `lines`, `tags`, and `sessions`.

Clocks: each line keeps its source's own timestamp, and `rx_us` says when
sai-server stored it.  A source's `lag_us` is the smallest `rx_us - t_us` of
its latest batch: its clock's skew plus how long its lines waited to go, up to
the 500ms the backlog is looked at.  Read a source's times with that in mind.

Lines older than 14 days are deleted, hourly; the ingestor's `retention-days`
pvo changes how long.

## Query

`GET /sai/logs` on sai-web, answered from the fleet log db like everything
else sai-web shows from sai-server's dbs.  The answer is JSON.

Arg|Meaning
---|---
`from`, `to`|UTC, ISO 8601 (`2026-10-14T08:01:02Z`, optionally with fractional seconds) or epoch ms.  With `hash`, they default to the build's window; otherwise to the last 10 minutes
`hash`|8 hex, the build, eg its job dir name; either case
`view`|`window` (the default without `hash`), `focused` (the default with it) or `hash`
`src`|comma-separated kinds or hosts, up to 8, `*` matches anything
`lvl`|the least important level to include: `err`, `warn`, `notice`, `info` or `debug`; `notice` and below include user lines
`grep`|a substring (not a regex) the line must contain
`limit`|the most lines to return, default 2000, at most 10000
`after`|the `next` from the previous answer, to get the lines after it

```
{"view":"focused","hash":"4d4fedab",
 "build":{"task":"4d4f...edab","runs":[{"run":0,"builder":"...",
          "start":"2026-10-14T11:58:05.000000Z",
          "end":"2026-10-14T11:58:35.000000Z"}, ...]},
 "from":"2026-10-14T11:57:05.000000Z","to":"2026-10-14T12:14:05.139170Z",
 "sources":[{"id":3,"kind":"sai-virt","host":"virt1","inst":"...",
             "lag_ms":12}, ...],
 "lines":[{"t":"2026-10-14T11:58:02.123456Z","src":3,"seq":991,"l":4,
           "h":null,"r":null,"x":"saiv_vm_reap: sai-vm-...-2: ..."}, ...],
 "truncated":true,"next":"1791979854493171:41"}
```

`sources` are the sources active in the window.  Lines come in UTC order.  `l`
is the line's `LLL_` bit: 1 err, 2 warn, 4 notice, 8 info, 16 debug, 1024
user.  An answer stops at `limit` lines or 8MiB, with `truncated` true and the
`next` to ask for the rest with.

A `hash` sai-server has never seen a line about is 404, unless `from` is given.

### Around an instant

Two more queries are for looking at one run's task log beside the fleet logs,
all at the same instant, see "The synchronised log view" below.  They go
anywhere in the run's window, a pane at a time, so they don't fetch the
window whole.  Times in them are UTC us, as numbers.

A task log's timestamps are its builder's monotonic clock.  The run's
`started_us`, when sai-server accepted its first step, is taken as the UTC of
its first row, so a row is placed `started_us` + how long after the first row
it is.  That's only as good as the builder's clock running steadily from
then, and runs from before `started_us` only have their start to the second.

`GET /sai/logs/sync?task=<uuid>[&run=<n>][&ts=<us>]` says what there is to
look at for a run, the latest one without `run`:

```
{"task":"C871...B0B1","run":0,"hash":"c871b0b1","taskname":"build-x",
 "builder":"bld1.linux-debian","start_us":1791900000000000,
 "end_us":1791900119500000,"running":false,
 "from_us":1791899940000000,"to_us":1791900179500000,
 "at_us":1791900010000000,
 "panes":[{"pane":"task","kind":"task"},
          {"pane":"sai-builder/bld1","kind":"sai-builder","host":"bld1",
           "lag_ms":7,"insts":2}, ...]}
```

`start_us` to `end_us` is the run, to its last row or, while it's going, now;
`from_us` to `to_us` is that padded by 60s.  `ts` is a timestamp from the task
log, and `at_us` is it in UTC, or the run's start without it.  The panes are
its task log, and each host with fleet log lines in the window: the run's
builder first, then sai-server, sai-virt, sai-power and other builders.  A
host's `insts` says how many instances of its daemon there were in the
window, eg, it restarted, and `lag_ms` is the latest one's.

Without `task`, it answers `{"allowed":true}` to anyone who may query, so a
page can tell whether to offer the view.

`GET /sai/logs/pane?pane=<pane>&at=<us>[&before=<n>][&after=<n>]` gives one
pane's lines either side of `at`: up to `before` at or before it and `after`
after it, 150 each by default and at most 500.  For the task log `pane` is
`task`, with the `task` and `run` of the sync query.

```
{"at_us":1791900030000000,
 "before":[{"i":581,"t":1791900030000000,"s":4,"l":1,"h":null,"r":null,
            "x":"..."}],
 "after":[...],"more_before":true,"more_after":true}
```

Both are in time order.  Fleet log lines have the source `s`, the level `l`,
the build `h` and run `r` they're tagged with, and the text `x`.  Task log
rows instead have the channel `c` and the base64 `b` of what the builder sent,
which can be part of a line or several.  `i` tells rows of the same time
apart.

### Who can query

It's decided by a grant of its own, eg `sai-logs`, which lws-login checks.
The front-end lwsws mounts `/sai/logs` separately from `/sai`, with an
lws-login interceptor whose `service-name` is that grant:

```
{ "mountpoint": "/sai/logs",
  "origin": "http://+/var/run/sai:/sai/logs",
  "interceptor-path": "/lws-login-sai-logs" },
{ "mountpoint": "/lws-login-sai-logs",
  "origin": "callback://lws-login",
  "protocol": "lws-login",
  "pmo": [{ "service-name": "sai-logs" }] }
```

lws-login refuses requests whose token doesn't hold the grant at level 1 or
more, or `*`, and stamps the result for sai-web in the `x-lws-login-*`
headers.  sai-web trusts those only if there's exactly one of each and the
request came on its unix socket (see the sai-web conf).  It answers if
`x-lws-login-state` is 4, a global admin, or if it's 2 (a user of the grant)
or 3 and `x-lws-login-service` says the grant was `sai-logs`, or what the
`fleetlog-service` pvo of sai-web's `com-warmcat-sai` protocol names.  It
refuses everything else with 403.  Each query is logged with the
`x-lws-login-sub` it was from.

Giving an account the grant, or taking it away, is all there is to who can
read the logs.

Without the mount, `/sai/logs` is judged by the grant of the `/sai` mount,
and lws-login stamps that grant's name, so sai-web refuses all but global
admins.  So does an lws-login too old to stamp `x-lws-login-service`.

A non-interactive client logs in like a browser does: a POST of username,
password and the current TOTP code to the auth server's `/api/login`, keeping
the session cookie it gets for its queries, and logging in again when it
expires.

`scripts/sai-logs.py` is such a client:

```
$ scripts/sai-logs.py --creds ~/my-sai-creds libwebsockets.org 'hash=4b9f390f'
$ scripts/sai-logs.py libwebsockets.org 'lvl=warn&src=sai-virt&from=2026-10-14T08:00:00Z&to=2026-10-14T09:00:00Z'
$ scripts/sai-logs.py libwebsockets.org 'sync?task=<uuid>'
```

It prints the JSON answer.  The credentials file holds the username, the
password and the TOTP secret, a line each; it logs in only when it has no
session cookie that still works, see its `--help`.

## The synchronised log view

The sai web UI shows a run's task log beside the fleet logs of the hosts
involved, all at the same instant, for an account that may query the fleet
logs.  It's a bar along the bottom of the page that opens into a pane, pushing
the rest of the page up; dragging the splitter above it sizes it.

It's opened at a line of a task's log, by right-clicking anywhere on the line,
or clicking its time.  It shows that try of the task, at the UTC that line
was, by the run's `started_us`.

There's a pane for the task log, and one for each host that logged anything
in the run's window, side by side, scrolling sideways if there are more than
fit.  Each has its name at the top: drag it onto another pane to move it
there, or double-click it to fold the pane into a narrow strip and back.  The
splitter after a pane sizes it, and folds it if it's made narrow enough.
Where the panes go and how wide they are is remembered by what they are, eg,
"the run's builder", not which host it was this time.

The line across the middle of every pane is the instant, which the bar at the
top gives in UTC to the us, and how far into the run it is.  Each pane has the
last of its lines at or before the instant on that line, and the lines that
came at the same time, eg, parts of what a builder read at once, are marked;
if its nearest line is earlier, its header says by how much.

The instant moves by:

 - the wheel or arrow keys over a pane, a line of that pane at a time; the
   other panes follow it in time
 - a finger dragged over a pane, the same
 - the time bar on the right, which is the run's window, padded by 60s: the
   run is the shaded part, and dragging or clicking goes anywhere in it

Panes fetch their lines either side of the instant as they need them, see
"Around an instant" above: left to right, only those that can be seen, and
after a jump, once it's stopped.  While the run is going, its window grows
as it goes on.

## Deploying

lws needs `LWS_WITH_LOG_SHIP` for every daemon, and `LWS_WITH_PLUGINS` with
`LWS_WITH_STRUCT_SQLITE3` for sai-server; `-DLWS_FOR_SAI=1` has them all.

sai-server's conf loads lws's plugins, and has the ingestor on its vhost,
beside `com-warmcat-sai`, eg

```
"global": {
	...
	"plugin-dir": "/usr/local/share/libwebsockets-test-server/plugins"
}
...
"ws-protocols": [{
	"com-warmcat-sai": {
		"database": "/srv/sai/sai-master",
		"link-key": "<the link key>",
		...
	},
	"lws-log-ingest": {
		"database": "/srv/sai/sai-master-logs.sqlite3",
		"secret": "<the link key>",
		"own-kind": "sai-server",
		"always": "sai-*",
		"service": "sai-logs"
	}
}]
```

`database` must be what sai-web reads: `<database>-logs.sqlite3` of sai-web's
`database`, unless sai-web's `fleetlog-db` pvo says otherwise.  `secret` is
the link key.  sai-server's old `fleetlog-retention-days` is now the
ingestor's `retention-days`.

sai-server first: the other daemons' old fleet log connections are refused
until they're updated, and the new ones until sai-server has the ingestor.
Then the builders, sai-power and sai-virt, in any order; each starts shipping
its lines when it next starts.  Nothing changes in their confs.  Add the
`/sai/logs` mount to the front-end lwsws, see above.

The old `<database>-fleetlog.sqlite3` isn't used any more, and can be deleted.

## Later

 - tagging more of sai-server's and sai-builder's lines about tasks
 - ephemeral VMs waiting for their queued lines to be acked before they power
   off, so the last moments of a VM, often the interesting ones, aren't lost
 - a journald follower for coredumps, OOM kills and service restarts
 - opening the synchronised log view at a fleet log line or a time, and
   panes for other runs' task logs at the same time
