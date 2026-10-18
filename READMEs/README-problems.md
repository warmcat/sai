# Problems

When each try of a task finishes, sai-server reads its log for warnings and
errors from configuring, compiling and linking, and for tests that failed,
including ones that only passed when they were run again.  sai-web adds up
what was found in every try of every task of the event, and shows it in the
problems pane, on the right of the tasks pane.

A build with nothing wrong in it adds nothing to the list, but it's still
counted, so it can be said that something happened on two of eight platforms,
or in all of them.

Idle tasks aren't read for problems, they report their own findings (see
[README-findings.md](README-findings.md)).

## What's recognized

Nothing here needs to know the languages, only how the tools lay out what they
say.  sai-server looks for:

|tool|how it looks|
|---|---|
|gcc, clang|`file:line:col: warning: ... [-Wflag]`, with the include chain and notes around it|
|MSVC, MSBuild|`file(line,col): error C2065: ...`, `x.obj : error LNK2019: ...`|
|rustc, cargo, clippy|`error[E0425]: ...` and `warning: ...`, then ` --> file:line:col`|
|CMake|`CMake Error at CMakeLists.txt:12 (message):` and its warnings|
|linkers|GNU ld's undefined references, duplicate symbols and missing libraries, lld's and Apple ld's undefined symbols|
|ctest|`12/80 Test #12: name ......***Failed`, with the test's output after it|
|cargo test, nextest|`test name ... FAILED` with its `---- name stdout ----`, and nextest's `FAIL [...] name` and retries|

The text of each is looked at without its ANSI colours.  What a tool prints for
one problem is kept together as a "stanza": eg, a compiler's include chain, the
diagnostic, then its notes and the source with a caret.  The start of the
stanza is kept with the problem, along with the file and line, the warning flag
or error code, the first thing it quoted and its message.

One line of code is one problem, however many things the tools said about it.
Eg, MSVC's `C2220: the following warning is treated as an error` and the
warning after it on the same line are one problem, an error, named by the
warning; so are two warnings about the same expression, or the same line of a
header warned about differently while compiling different files.  Each thing
said is kept in the problem's excerpt, unless it was said the same before.
Notes are kept with what they're about, eg, `note: previous definition is here`
in a header, after its own include chain, or MSVC's `note: see previous
definition of 'X'`.  This isn't done for what linkers say, where the problem is
the symbol, nor for tests.

In a failed test's output, the first sanitizer report, UBSan runtime error,
assert or Rust panic is taken to be what went wrong.

Lines of `make` and `ninja` only saying something failed, `collect2`'s and
clang's notes that the link failed, MSBuild's summaries and cargo's `could not
compile` are left out, they follow from problems found anyway.

A step that failed with nothing recognized in it still says something: the last
lines it printed are kept as a problem, with the one most like an error as its
message, eg, `/bin/sh: 1: make: not found`.

The git mirror and checkout steps aren't read.

## Tries and tests run more than once

Every try of a task is read, and what it found is kept as that try's, so
a test that's flaky, or a build that was fixed by changing the builder, eg,
installing a library it needs, shows how often it went wrong: eg, `in 1 of 3
tries`.

Tests may be run more than once in a try too, eg, with ctest's
`--repeat until-pass:3`.  Every attempt is counted, so a test that failed and
then passed is known to be flaky the first time it happens.  How often it fails
is counted over all its attempts in every try of the builds it failed in, eg,
`failed 2 of 61 runs (3%), passing on retry`, or `failed all 42 times it ran`.
The times it passed in other builds aren't counted: that it only fails on some
platforms or configurations is said by which builds it was in, it's not chance.

## Grouping

The same problem is reported differently by different compilers, on different
builders whose checkouts are in different places.  Problems are grouped:

 - a test, by its name
 - a missing or duplicate symbol, by the symbol without what the platform
   decorates it with, eg, Apple's leading `_` or MSVC's `__imp_`
 - otherwise, one in a file, by where it is: paths are taken relative to the
   checkout, and paths that are the same file are the ones that match from the
   right, whatever is on the left.  Problems in the same file within a few lines
   are the same problem if they're on the same line, have the same warning flag
   or error code, quote the same thing, or say it in similar words.  Then the
   groups in the same file with the same warning flag or code, affecting the
   same builds, are one group about all those places.
 - one with no file, by its tool and what it says, without what it quoted or
   any numbers

## Which builds

A build is its configuration (the task name) and its platform's OS,
architecture and toolchain, eg, `noclient` on `freebsd/aarch64/llvm`.  Which
builds a problem was in is said in the fewest of those words that say exactly
those builds, out of the ones that finished, eg:

 - `every build (30)`
 - `all noclient builds (8)`
 - `every build on freebsd (4)`
 - `every msvc build that are all noclient builds (2)`
 - `2 of 8 platforms on default: rocky9/aarch64-a72a55-rk3588/gcc, ...`

For a test, it's out of the builds that ran it.

## History

What each problem is found by in other events doesn't include its line, since
that moves as the code around it changes.  For each try, sai-server keeps that
in the main db's `problem_history`, with the event and when it was, for 400
days, after the events' own dbs are pruned.

A problem seen in other events of the same repo in the last 180 days says how
many, out of the events looked at for problems in that time, and when it was
first and last seen: eg, a test that has been failing once a week and passing
when it's run again, for months.

A problem is new if, of the events of the same branch before this one that
were looked at for problems, up to 100 of them in the last 180 days, it was in
none, or only in the latest ones, one after another.  It must have not been in
at least one of them, so nothing is new when there's no history to tell.  If
it was in an earlier event, then not in one after that, it's not new, it comes
and goes.  The summary's `new` says how many events in a row, with this one,
it's been in.  A new problem isn't flaky, even if it passed on retry: it's
only just appeared, there's nothing yet to say it comes and goes, it may be
here to stay.  So it's counted as an error.  But if it didn't happen every
time here, eg, it failed in only some of the times the test ran, chance is in
it too and it's still given the dice.  Any event that was looked at counts,
even if it didn't get to run the builds the problem is in.

## Storage

In the event's db, `problems` has a row for each different problem found in
a try, and `problem_runs` a row for each try that was read, even if it found
nothing.  A try's rows are replaced if it's read again.  Removing a task's tries
removes them, and the event's db going takes them with it.

At most 400 different problems are kept from one try; any more are only
counted.  The start of each stanza is kept, up to 24 lines or 2KiB.

Tries that finished without being read, eg, while sai-server wasn't running,
and those read by an older version of the parser, are read by going through
the events with dbs, newest first, after sai-server starts and once a day after
that.  They're read more slowly than tries finishing now, so a backlog doesn't
slow anything else down.

## The problems pane

The pane is green if nothing went wrong in the builds that finished, red if
there are errors, and yellow if there are only warnings or flaky tests.

Icons in the top left corner of a group say it's an error or a warning, or,
with a dice, that it only happens some of the time: a test that failed and then
passed, or failed in only some of the times it ran in the builds it failed in,
or a problem seen in only some of the tries of the same builds.  A new bug says
it's new.

Clicking a group opens it: each place it was seen, with the build, try, builder
and when, and the start of what the log said there.  Clicking one of those
opens that task's log at the lines of the stanza.  As with the other panes, the
splitters between them can be dragged, and are remembered in the browser; on a
touch screen, hold one for a moment before dragging it.

## From the command line

`scripts/sai-problems.py` asks a sai-web for the same things the problems pane
shows, over the same websocket, so it's handy for looking into an event from a
shell, or for an agent.  What it shows is public, so it needs no account.

```
$ scripts/sai-problems.py libwebsockets.org events libwebsockets main-dev
$ scripts/sai-problems.py libwebsockets.org problems latest:libwebsockets:main-dev
$ scripts/sai-problems.py libwebsockets.org excerpts <event uuid> 1,2,3
$ scripts/sai-problems.py libwebsockets.org tasks <event uuid>
$ scripts/sai-problems.py libwebsockets.org log <task uuid> [<try>]
```

`problems` lists each group with its id, severity, phase, test or file, which
builds and how often, its history in other events, then where it was seen:
the occurrence's `uid`, the task uuid and try, and the line in the task's log
it starts at.  `excerpts` gives the start of what the log said there, for the
uids given; `log` gives the whole log of the task's try, as the builder sent
it, eg, to see what came before.  `--json` prints sai-web's own JSON instead.

The browser sends, and the tool sends the same:

|schema|asks for|
|---|---|
|`com.warmcat.sai.problems`, `event_hash`|the event's summary, answered with the same schema|
|`com.warmcat.sai.problem_excerpts`, `event_hash`, `uids` ("1,2,3")|the occurrences' excerpts, answered with the same schema|
|`com.warmcat.sai.eventinfo`, `event_hash`|the event and its tasks, as `sai.warmcat.com.overview`|
|`com.warmcat.sai.taskinfo`, `task_hash`, `logs`: 1, `run`|the task, then its log as `com-warmcat-sai-logs` rows|

In the summary, `sev` is 0 for a warning, 1 for flaky and 2 for an error, and
`phase` is 0 to 4 for configuring, building, linking, testing and other.

## Testing the parser

`tests/problems` has logs from each of the tools with what should be found in
them, and an event made of several tries whose summary is checked.  It's built
with sai-server and run by `ctest`.
