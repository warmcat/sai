# The planner

sai-server decides how each step of a task runs on a builder from what the
same step took there before.  Nothing about a builder's capacity is
configured: how many cores it really has, how many are left when a VM has
some, whether a step even uses `SAI_PARALLEL`, and how much memory it wants
at each `-j`, all differ per step and per builder platform, and all show in
the measurements.  So sai measures, and decides from the measurements.

This is the start of it.  Today the planner chooses each step's
`SAI_PARALLEL`, which was always 2; the builder still decides for itself how
many steps it runs at once.

## What's recorded

Every step a builder completes sends sai-server a row for the build metrics
db, `<sqlite3-path>-build-metrics.sqlite3`, with

|column|meaning|
|---|---|
|`key`|hash of the builder platform name, the task name and the project: all the runs of this task on this platform, whatever ref they built|
|`step_hash`|hash of the step's line of the build; `git-mirror` and `git-checkout` for sai's own two steps|
|`ref`|the ref it built, so a lookup can prefer rows from the same ref|
|`hardware`|the machine the platform runs on, see "Builder hardware" in the main README|
|`parallel`|the `SAI_PARALLEL` it ran with|
|`wallclock_us`, `us_cpu_user`, `us_cpu_sys`|how long it took, and how much cpu it used doing it|
|`peak_mem_rss`|the most memory its process tree had resident at once|
|`stg_bytes`|the job dir's size when it finished|
|`cotenants`|the most other steps that were running at once on any platform of the same sai-builder while it ran, or -1 from a builder from before this, which couldn't say|
|`cotenant_parallel`|the most those other steps' `SAI_PARALLEL`s added up to, or -1|

A row with `cotenants` 0 ran alone on its machine, so its times are the
step's own.  Rows with company, or that can't say, are still useful, but
their times may include whatever else was running, so the planner trusts
them less.

Rows from a builder that hasn't been updated yet arrive in the old shape,
identifying their step by its index, and are keyed from their task as they
arrive, as the rows from before the change were when sai-server started.

Keying on the step's text rather than its place in the build means editing
the build to add or remove a step leaves the other steps' history where it
is, and keying without the ref means a new branch starts with its task's
history from the branches before it.

The newest 8 rows are kept for each key, step and `SAI_PARALLEL`, so every
point on a step's `-j` curve is kept, however long ago it was measured.

## Choosing SAI_PARALLEL

A step's `-j` curve is unknown to begin with: the planner finds it by trying.
When a step is offered to a builder platform, the planner looks at the rows
for that step on that platform:

 - the first time, there are none, and the step runs at `-j2` as it always
   did

 - after that, it picks the fastest `-j` it has rows for, averaging the newest
   4 rows of each.  If any rows of a `-j` ran alone, only those count for it.
   A `-j` whose rows all had company needs at least 2 of them before it can
   beat another.

 - a `-j` within 5% of the fastest is a tie, and the lowest `-j` in the tie
   wins.  That's how a step that doesn't use `SAI_PARALLEL`, or has run out
   of cores, settles low instead of wandering about in the noise

 - while the fastest it has seen is also the highest it has tried, it tries
   the next rung up once; the rungs are 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48
   and 64.  When the lowest it has tried is the fastest, it tries the next
   rung down once.  So a step that scales climbs until it stops gaining, and
   one that doesn't costs two extra runs to find that out

 - when a neighbour of the chosen rung hasn't been run for 14 days, it's run
   again, so the curve follows the code as it changes

Only one task at a time tries a new rung for a given step: otherwise every
task of a busy event would jump up a rung together on no new information.
The rest run at the best that's known.

Going up a rung, the memory estimate the builder admits the step on is the
chosen rung's, scaled by the increase in `-j`.  If the builder refuses it
because that much isn't free, the rung is left alone for an hour and the
step is offered again at what's known to work.  A builder that refuses
because it's full, which says nothing about the `-j`, just ends the attempt,
and a later offer of the step may try the rung again.  If the step fails while
trying a new rung, which may be the `-j`'s doing, eg, tests that ran out of
ports, that rung is left alone for a week, and the task's log says so next
to the failure.

The task's log says what the planner did for each step and why, eg

```
planner: step 4: -j8, fastest of 5 tried (41.2s from 4 runs alone)
planner: step 4: trying -j12 to measure it, best so far is the highest tried (-j8, 41.2s)
```

## What's next

The measurements are kept per builder platform, and the `hardware` column
says which platforms share a machine.  That's what the rest needs:

 - the server deciding how many steps a machine runs at once, across all its
   platforms, from what those steps took, instead of each platform's fixed
   `instances`

 - running a step alone on its machine on purpose when nothing comparable
   has been measured, so the measurement is clean, and saying so in the UI

 - a coarse profile of a step's cpu and memory over its run, so steps whose
   peaks come at different times can be overlapped
