# Closing the loop for agents: plan

This is a plan, not a description of what sai does today.  It collects what
would let a coding agent use sai as its own CI: push work it has tested
locally, start just the builds it cares about, be told what they found, and
fetch what it needs to act on it, without polling and scraping.  It is split
into work items that can be handed out separately; each says what it builds
on, what "done" means, and what it depends on.

It came out of a session where an agent watched lws `main-dev` events and
fixed what the problems pane found, with a person doing the pushes.  The
friction it met is the motivation for each item.

## The flow it is for

 - Only the top level agent directing the work pushes to sai, after local
   build and test of what it and any helpers it delegated to produced.
   Helpers never push.

 - It pushes to a branch that doesn't feed `main` (see A).  Nothing runs
   until it asks.

 - It starts the tasks it is interested in: eg, to confirm a fix for a
   flaky test on the one platform that showed it, run that, several times,
   before spending the whole fleet on a full event.

 - It is told as problems are found and as tasks finish (B), reads them as
   JSON (C), fetches the end of a failing test's output (D), and has what it
   needs to build the same thing locally (E).

 - A commit claiming to fix a problem says so (F); the events after it say
   whether the claim held.

 - Only a build that ended with no problems is promoted to `main`, or to an
   agent-main branch.  So whatever problems a later event of the agent's
   branch has are, by construction, ones `main` doesn't: no baseline diff is
   needed.  When the fleet is busy, the last promotion may be from a while
   ago; that's fine, it's still a known clean point.

## What already exists to build on

|piece|where|use here|
|---|---|---|
|scratch branches, `refs/heads/_*`: the push is recorded in `pushes`, nothing is scheduled|`s-notification.c`, README-adhoc-builds.md|the "record but don't run" half of A|
|ad-hoc builds: a single-task event cloned from a seed task, `adhoc = 1`, outside the branch's CI history and status badge|README-adhoc-builds.md|how events outside normal CI are kept apart|
|task states `SAIES_NOT_READY_FOR_BUILD`, `SAIES_PAUSED`|`src/common/include/private.h`|a held task is one of these, or a new state beside them|
|task reset, event delete (admin)|sai-web|A's "start" is close to a reset of a held task|
|problems: parsing, grouping, history, `new`, flaky|README-problems.md|B, C, F, G build on its tables|
|`scripts/sai-problems.py` with `events`, `problems`, `excerpts`, `tasks`, `log`, and `--json` for sai-web's own JSON|README-problems.md|the client side of B, C, D|
|fleet logs and `scripts/sai-logs.py`|README-logs.md|G's evidence|
|findings: grouped bugs, news, mail|README-findings.md|the model for F's claims and G's flags|
|lws `LWS_WITH_COMPRESSED_BACKTRACES`|lws `CMakeLists.txt`|H, for lws's own code|

## A. A manual branch: an event whose tasks wait to be started

A push to a magic branch, eg `refs/heads/sai_manual` (or a prefix such as
`sai_manual/<name>`, so more than one agent can have one), is handled like a
normal push: the hook POSTs it, sai-server expands `.sai.json` into an event
with all its tasks.  But every task starts out held, and none is given to a
builder until asked.

Then an api on the event starts selected tasks: by task uuid, or by
configuration and platform, eg `{"config": "ipv4-off", "platform":
"rocky9/x86_64-amd/gcc"}`, or "all".  It should take a repeat count, ie, run
this task's try N times, so a flake fix can be confirmed before a full event.
An agent should also be able to start a task with its ctest narrowed, eg,
`ctest -R <regex> --repeat until-fail:N`, since often it's one test on one
platform that matters.  That last part needs the substituted build steps to
have a place for it, eg, a `${ctest_args}` the server can set per start;
ad-hoc builds already let the steps be edited, which is the general case.

Notes:

 - Held tasks must not count as stuck, waiting or failed in any summary,
   badge or "is the event finished" logic.  An event with held tasks is
   finished when all its started tasks are.

 - These events are kept apart from the branch CI history the way ad-hoc
   ones are: not in the status badge, not a baseline for `new`.  Problem
   history should still record them, flagged, so F can follow a claim
   through them.

 - The start api is an admin, or a new "agent" grant, operation, like task
   reset.  The push side is gated by the git server: the agent's key may only
   push refs under the magic prefix (gitolite rules, README-gitolite-hook.md).

 - A newer push to the same manual branch should cancel the older event's
   running and held tasks, so iterating doesn't load the fleet with stale
   work.  Manual events should be scheduled behind normal CI ones.

Done when: an agent can push to its manual branch, see an event with every
task held, start two named tasks with a repeat count of 5, and get their
results, with nothing else built and the branch's CI status untouched.

## B. A websocket stream of changes, for agents

A subscription on sai-web, filtered by project, branch or event, that pushes
JSON messages as things change:

|message|when|
|---|---|
|`event.new`|an event was created for the filter, with its uuid, ref, hash and whether it is manual|
|`event.state`|counts of held / waiting / building / ok / failed changed, at most every few seconds|
|`event.done`|every started task has finished, with the final counts|
|`event.gone`|deleted, or superseded by a newer push to the same manual branch|
|`task.done`|a task's try finished, with its uuid, config, platform, try and result|
|`problem.new`|a problem group appeared in the event, with its summary as in C|
|`problem.changed`|a group gained builds or occurrences, or its severity or flaky state changed|
|`claim.result`|F: a claimed fix held, or didn't|

Messages carry the event uuid and the commit hash, so a client never has to
map one to the other.  An event must be findable by its commit hash too, eg,
`latest` already works by branch; `hash:<sha>` should work as well.

The browser's own websocket protocol stays as it is; this is a separate,
documented, stable protocol, versioned in its schema names.  It is public
data, so it needs no account, but it should be rate limited per connection.

`sai-problems.py` gains a `watch` command that prints one JSON line per
message, which is directly usable as an agent's notification stream.

Done when: an agent subscribed to a branch is told of a new event, each new
problem group, and the event finishing, without polling anything.

## C. JSON everywhere, selectable

`sai-problems.py --json` already prints sai-web's own JSON.  Make it the
documented contract, the same shape the stream in B uses, for every command:
`events`, `problems`, `excerpts`, `tasks`, `log`.  For `problems`, each
group's id, severity, phase, kind, test or file, the builds it was in (each
as config and platform), the failure rate, `new` and history, and each
occurrence's uid, task uuid, try and log line.  Schemas are named and
versioned; text output stays the human default.

`log --json` gives the log as JSON lines with the channel (stdout, stderr,
sai's own) and time, ANSI escapes and the builder's `>saib>` prefixes
separated out rather than mixed into the text.

Done when: an agent never has to parse sai's text output.

## D. The end of a test's output, not the start

An excerpt is the start of the stanza, 24 lines or 2KiB.  For a failed test,
the start is the least useful part: the reason is usually at the end.  This
is true for people reading the problems pane as well.

 - Keep the last N lines of a test's stanza as its excerpt, or the start and
   the end, with a marker for what was left out.  Compiler diagnostics keep
   their start, where the diagnostic is.

 - Add "the whole output of test X in task T try R", including each time
   ctest ran it again, as one request that ends when it has sent it.  Today
   an agent fetches the whole task log (`log` doesn't end while the event is
   still building, other tasks' updates keep the connection busy) and greps
   for `Test #N`.

 - Logs can be hundreds of thousands of lines.  Both of these must be served
   from what the parser already recorded, the stanza's line range, without
   scanning the whole log for each request; and capped, eg, to the last
   64KiB, with the size said.

Done when: an agent gets the complete output of a failing test, all its
runs, from one request that finishes, whatever else the event is doing.

## E. Exact reproduction details in each task

To reproduce a failure locally, an agent today reads `.sai.json` to rebuild
the configuration's cmake line, and the log's banner for the tls library and
its version.  sai knows all of it when it runs the task:

 - the fully substituted steps the builder ran (the ad-hoc dialog already
   has these)
 - the platform's OS, version, architecture, compiler and version
 - the versions of the main dependencies the build found, eg, the tls
   library, from cmake's cache or the builder's package database
 - the environment sai set for the steps

The builder reports what it can at the start of a task, as a structured
record kept with the task and returned in C's `tasks` and with each problem
occurrence.  This needs every builder upgraded, so it can come in over time:
a task from an older builder just has less in it.

Done when: from a problem occurrence alone, an agent can make a local build
of the same configuration, or knows which part it can't match (eg, a
platform it doesn't have, where A's targeted start is the answer).

## F. Commits that claim to fix a problem

A problem group gets a stable id across events, eg `saiprob-<uuid>`: the
"meta-problem", following it through history as `new` and the history
counts already do.  It's shown in the problems pane and in C and B.

A commit that means to fix it says so in a trailer:

```
Supposed-to-fix: saiprob-<uuid>
```

sai-server reads the trailers of the commits new in each event (the hook can
send them, or the builder that mirrors the tree can), and records the claim
against the meta-problem.  In later events of that branch which contain the
commit:

 - if the problem is gone from the builds it was in, once enough of them
   have run that it would have shown (more for a flaky one), the claim is
   marked as held
 - if it's still there, the problem shows the commit that claimed to fix it,
   and `claim.result` says it failed

A claim on a flaky problem can only be "not seen in N runs", so it should say
how many runs that was.

Done when: an agent can put the trailer on its fix, push, and later be told
whether the problem it named went away, without having to remember what it
claimed.

## G. "Infra?" flags

Some problems are the fleet, not the code: eg, an ESP32 dropping off its
wifi with "too many unacked frames", MSBuild's `MSB4166: Child node exited
prematurely`, a VM reaped, a git fetch failing, a builder restarting.  sai
knows many of these from the fleet logs and the builder's own reports.

Mark a problem occurrence as "infra?" when there's such evidence around it
in time, on that builder, with the evidence linked.  Known patterns, like
MSB4166, can be matched directly.  It doesn't make the problem go away:
someone still has to follow it up, so it stays visible, but an agent can
leave it to a person rather than looking for a bug in the code.

Done when: today's two examples, the ESP32 wifi drop and MSB4166, would each
have been flagged with the evidence beside them.

## H. Backtraces from crashes

A test that crashed or asserted on a board says where only if it printed it.
Collecting a backtrace when a test process dies abnormally, symbolized, into
the task's log or as an artifact, would often make reproducing it
unnecessary.

It should not depend on how the project was built: eg, the builder notices a
child died on a signal or an abort and uses the platform's own way to get a
stack (core files and the platform debugger, or the OS's crash reports on
macOS and Windows), and it should work for Rust as well as C (Rust panics
already print a backtrace with `RUST_BACKTRACE=1`, which sai could set).
lws has `LWS_WITH_COMPRESSED_BACKTRACES` for its own code, which can be a
source when present, but the general case can't rely on it.

Done when: an assert in a test on, eg, a Linux and a macOS builder gives a
symbolized stack in the problem's occurrence.

## I. A record of what an event's first failing commit was

`new` says how many events in a row a problem has been in.  It should also
say the first commit it was in, and the last event it wasn't, so the range
of commits that brought it is known without working it out from git.  With
an agent pushing each commit to a manual branch, that range is often one
commit.

Done when: a new problem's summary names the commit range it came in.

## Order

A and B unlock the most: A makes an agent's pushes cheap and targeted, B
removes polling.  C is mostly documenting and finishing what `--json`
started, and B depends on its shapes, so it should come first or with B.
D and F are independent of each other and of A.  E needs builder upgrades
and can land a field at a time.  G, H and I are independent.

|item|depends on|size|
|---|---|---|
|C. JSON contract|-|small|
|B. change stream|C|medium|
|A. manual branch, start api|-|large|
|D. end of test output|-|medium|
|E. reproduction details|builder upgrades|medium, incremental|
|F. fix claims|meta-problem ids|medium|
|G. infra flags|fleet logs|medium|
|H. backtraces|builder|medium per platform|
|I. first failing commit|problem history|small|
