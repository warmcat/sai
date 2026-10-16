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
the 8-character job dir name, `xxxxxxxx`: the first 4 and last 4 characters of
the task uuid (`saib_task_jobdir_vn()`).  That's per task, so all runs of a
task share it, and the run number is kept beside it.

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

## Sources

sai-server, sai-builder, sai-power and sai-virt each keep the lws log lines
they emit.  sai-web isn't a source, its logs don't bear on the problems this is
for.  A source is identified by

 - `kind`: `sai-server`, `sai-builder`, `sai-power` or `sai-virt`
 - `host`: the host name, or for a builder in a sai-virt VM, its VM name
 - `inst`: 16 random hex characters chosen when the daemon starts

`inst` matters because sai-virt reuses VM names: `sai-vm-...-2` today is not
the same machine as `sai-vm-...-2` an hour ago, and a restarted daemon starts
its sequence numbers again.

A source's `host` can change.  A builder starts shipping as soon as it has an
lws context, before it is operational, so what goes wrong before then, eg, a
sai-virt VM that can't reach sai-virt or resolve its name, is here too.  In a
sai-virt VM that is before it knows which VM it is, so it ships under the host
name in its conf, and takes the VM's name once sai-virt has told it.  The
lines it shipped before are the same daemon's, so sai-server renames the
source, and they are found under the VM's name too.

The lines kept are the ones the daemon logs at its configured level (`-d`);
raising a daemon's level is how to get more detail into the fleet logs.  What
isn't an lws log line, like coredumps, OOM kills, systemd restarts, isn't here
yet.

## Capture

Each daemon sets its lws log level with `saicom_fleetlog_set_log_level()` (or,
where the level came from `-d` already, `saicom_fleetlog_hook_current()`)
instead of `lws_set_log_level()`.  That installs an emit function that still
writes each line where it went before (stderr / syslog / a file), and also
copies it into a queue in memory, with:

 - its UTC time in microseconds
 - its level, its `LLL_` bit
 - a per-source sequence number, from 1
 - its tag, if it has one, see below

lws's own local time and level prefix is taken off the line, and anything
that's not printable ASCII is replaced with `?`, so whatever was logged is
harmless in JSON and in a browser.  Lines are cut at 1024 chars.

The hook does nothing else: no networking, no logging, so it can't recurse or
stall whatever was logging.  A sul looks at the queue every 500ms.

The queue holds up to 4MiB of lines not yet stored by sai-server.  Past that,
the oldest lines not yet sent are dropped, and a line saying how many takes
their place.  It's in memory, so lines not yet sent are lost if the daemon
dies, but not if the connection to sai-server does.

Lines the lws log spew handling swallowed never reach the hook; its
"swallowed N logs" summary line does, so the gap is visible.

### Tagging

A line is tagged by having `[b:xxxxxxxx]` or `[b:xxxxxxxx/rN]` in it, eg

```
lwsl_notice("[b:%.4s%s/r%d] %s: allocated to %s\n", uuid, uuid + 60, run,
	    __func__, builder);
```

The hash in a tag may be either case, it's kept in lowercase.

As well, when sai-server stores a line with no tag, it looks for a task uuid,
64 hex of either case, in it and tags it from that.  sai-server already logs task
uuids in many places, so this tags a lot without any logging changing.  It
also remembers which task uuid each job dir name belongs to, which is how a
query goes from a job dir name to the task's runs.

## Shipping

sai-builder, sai-power and sai-virt each open a Secure Streams ws connection
of its own to the first sai-server in their conf, stream type `sai_fleetlog`,
separate from their task / power link.  So a big upload doesn't hold up task
traffic, and a problem on the task link doesn't take its own log with it.  Its
url is the daemon's link to that server, with the last path part changed to
`fleetlog`, eg `wss://sai.example.com:4444/sai/fleetlog`.  As on the other
links, the first message is the link key (`com.warmcat.sai.linkauth`).

Then, a batch at a time, up to about 48KiB of JSON:

```
{"schema":"com.warmcat.sai.fleetlog",
 "kind":"sai-builder","host":"sai-vm-debian-13-nocloud-amd64-BASIS-2",
 "inst":"9f2c1e0a7b3d4c55",
 "lines":[{"seq":1041,"t":1791876551123456,"l":4,"h":"4d4fedab","r":2,
           "x":"[b:4d4fedab/r2] reject task ...: powering down"}, ...]}
```

and sai-server acks the highest seq in the batch once it has stored it:

```
{"schema":"com.warmcat.sai.fleetlog.ack","inst":"9f2c1e0a7b3d4c55","seq":1041}
```

Only then are the batch's lines freed.  One batch is outstanding at a time.  A
batch sent on a connection that went away, or not acked within 30s, is sent
again; sai-server ignores lines it already has, so nothing is stored twice.

sai-server's own lines are stored directly, without a connection.

sai-server checks every batch as if it came from anywhere, even over an
authenticated link:

 - `kind` must be one of the four; `host` is up to 64 of `[A-Za-z0-9._-]`;
   `inst` 16 lowercase hex
 - `l` must be a single real `LLL_` bit
 - `h` empty or 8 hex of either case, kept in lowercase, `r` within -1..1000
 - `x` up to 1024 chars, `t` within a day of the server's own clock
 - a message is at most 256KiB, or the connection is dropped
 - each source may store 12000 lines a minute; past that its lines are dropped,
   and a line saying how many is stored

A malformed line is refused, and a line saying how many were stored instead.

## Storage at sai-server

Next to the events db, in `<database>-fleetlog.sqlite3`, eg
`/srv/sai/sai-master-fleetlog.sqlite3`, in WAL mode so sai-web can read it while
sai-server writes:

```
CREATE TABLE sources (src_id INTEGER PRIMARY KEY, kind TEXT, host TEXT,
		      inst TEXT UNIQUE, first_us INTEGER, last_us INTEGER,
		      lag_us INTEGER);
CREATE TABLE lines (id INTEGER PRIMARY KEY, src_id INTEGER, seq INTEGER,
		    t_us INTEGER, rx_us INTEGER, lvl INTEGER, hash TEXT,
		    run INTEGER, line TEXT);
CREATE UNIQUE INDEX lines_src_seq ON lines (src_id, seq);
CREATE INDEX lines_t ON lines (t_us);
CREATE INDEX lines_src_t ON lines (src_id, t_us);
CREATE INDEX lines_hash ON lines (hash, t_us);
CREATE TABLE builds (hash TEXT PRIMARY KEY, task_uuid TEXT,
		     first_us INTEGER, last_us INTEGER);
```

Lines sai-server adds itself, eg to say some were dropped, have no `seq`.

Clocks: each line keeps its source's own timestamp, and `rx_us` says when
sai-server stored it.  A source's `lag_us` is the smallest `rx_us - t_us` of
its latest batch: its clock's skew plus how long its lines waited to go, up to
the 500ms the queue is looked at.  Read a source's times with that in mind.

Lines and builds older than 14 days are deleted, hourly.  The
`fleetlog-retention-days` pvo of sai-server's `com-warmcat-sai` protocol
changes how long.

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

sai-server first: until it has this, it refuses connections to `/fleetlog`,
and the other daemons keep retrying.  Then the builders, sai-power and
sai-virt, in any order; each starts shipping its lines when it next starts.
Add the `/sai/logs` mount to the front-end lwsws, see above.

## Later

 - tagging more of sai-server's and sai-builder's lines about tasks
 - ephemeral VMs waiting for their queued lines to be acked before they power
   off, so the last moments of a VM, often the interesting ones, aren't lost
 - a journald follower for coredumps, OOM kills and service restarts
 - opening the synchronised log view at a fleet log line or a time, and
   panes for other runs' task logs at the same time
 - moving the emit hook and queue into lws, once settled
