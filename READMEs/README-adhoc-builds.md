# Ad-hoc builds

Normally every push to a watched repo makes the git hook POST a signed
notification carrying the tree's `.sai.json`, and sai-server expands that
into an event with one task per configuration per platform.

Ad-hoc builds are the manual counterpart: from the web UI an admin picks an
existing task as a seed, optionally edits its build steps, chooses which
branch's head to build, and sai-server creates a new event containing just
that one task.  It runs on the real builders like any other task, but it
doesn't count as CI for the branch.

The intended workflow is

 - push the tree you want to test to a scratch branch whose name begins
   with `_`, eg, `_temp`

 - in the web UI, find a recent event for the project, right-click the task
   for the build dimension and platform you want, and choose
   "Ad-hoc build from this task…"

 - the dialog defaults to the most recently pushed `_` branch; adjust the
   build steps if needed and click Schedule

 - a new single-task event appears for the scratch branch, marked with a
   dashed border

## Scratch branches

A branch whose name begins with `_` is treated as scratch: sai-server records
the push (repo, ref, hash) in its `pushes` table but does not schedule the
`.sai.json` for it.  So pushing to `_temp` costs nothing on the builders,
and the ad-hoc dialog can offer "the head of `_temp` as last pushed".

The git hook must still fire for `_` branches for this to work; it is
sai-server that decides not to CI them.

The `pushes` table is updated for every authenticated notification, whatever
the ref, so an ad-hoc build can also be pointed at, eg, `refs/heads/main`.
For a ref with no recorded push (an installation that predates the table),
sai-server falls back to the newest non-deleted event on that ref.

## What the new task inherits

From the seed task: the platform, the `.sai.json` configuration name (build
dimension), package deps, artifacts list and log limit.  So the new task is
shown and searched like any other task of that dimension.

From the seed task's event: the repo name and its fetch / web URLs.  The
browser never supplies a repo or a hash, only the seed task uuid, a ref and
the build script; sai-server resolves the ref to a hash itself.

The build script is the seed's *expanded* per-platform script, ie, with the
configuration's `${cmake}` etc already substituted, which is what the builder
actually ran.  Edit it freely; it is split into steps one per line, as usual.
It is limited to 4000 bytes.

Everything else is fresh: uuids, artifact nonces, state.  The seed's event is
not modified.

## Authorization

Only browsers whose websocket was established with the front-end lws-login
interceptor's admin verdict (`x-lws-login-admin: 1`, ie, the user holds the
service's admin grant or the `*` grant) see the context menu entry, and
sai-web refuses `com.warmcat.sai.cloneinfo` and `com.warmcat.sai.taskclone`
from anyone else, the same way it does for task reset and event delete.
See README-auth.md and the comments in `etc-sai-EXAMPLE/web/conf.d/unixskt`.

## Effects on the rest of sai

Events created this way have `adhoc = 1` in the `events` table (the column is
added at startup on existing databases).  Ad-hoc events are

 - excluded from the notification dedupe on hash, so a later real push of the
   same commit still gets its normal CI event

 - excluded from the project head status badge (`/status/<project>`)

 - shown with a dashed border in the sidebar event list, the event header
   and the event summary, with an "ad-hoc" tag in the header

They can be reset, deleted and inspected exactly like any other event.

## Protocol

Browser -> sai-web:

```
{ "schema": "com.warmcat.sai.cloneinfo", "uuid": "<seed task uuid>" }
```

sai-web -> browser (answered locally from the databases):

```
{ "schema": "com.warmcat.sai.cloneinfo",
  "seed_uuid": "...", "repo_name": "...", "ref": "<seed event ref>",
  "taskname": "...", "platform": "...", "build": "...",
  "refs": [ { "ref": "refs/heads/_temp", "hash": "..." }, ... ] }
```

`refs` lists the project's `_` branches, most recently pushed first.

Browser -> sai-web -> sai-server:

```
{ "schema": "com.warmcat.sai.taskclone",
  "seed_uuid": "...", "ref": "refs/heads/_temp", "build": "..." }
```

sai-server announces the new event with the usual `sai-eventchange`, so it
appears in connected browsers without a reload.
