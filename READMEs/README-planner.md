# The planner

sai-server decides how each step of a task runs on a builder from what the
same step took there before.  Nothing about a builder's capacity is
configured: how many cores it really has, how many are left when a VM has
some, whether a step even uses `SAI_PARALLEL`, and how much memory it wants
at each `-j`, all differ per step and per builder platform, and all show in
the measurements.  So sai measures, and decides from the measurements.

This is the start of it.  Today the planner chooses each step's
`SAI_PARALLEL`, which was always 2, and has a step measured alone on its
machine when nothing comparable has been; the builder still decides for
itself how many steps it runs at once.

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
point on a step's `-j` curve is kept, however long ago it was measured.  The
newest row from a run alone is kept as well, whatever its age: it's the
reference for its `-j`, see "Measuring alone", and on a busy machine the rows
after it mostly have company.

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

## Measuring alone

Everything above compares a step's runs at one `-j` with its runs at
another, and a run that shared the machine with other steps says as much
about them as about itself.  So the first run of a step at each `-j` is
made alone, as the reference the rest is compared with, and after that the
step runs with company like anything else, the later rows supplementing the
reference.

The server knows which platforms share a machine from the `hardware` each
builder platform gives when it registers, see "Builder hardware" in the main
README: it groups the live platforms by it, and sai-web shows each
platform's group as a badge, eg, `hw3`, so you can see what the planner
thinks shares what.  The index is just the order the server first saw each
machine since it started.

When a step is offered at a `-j` that has no row from a run alone on that
platform, the planner holds the step's machine for it: nothing new is
offered to any platform on the machine, including idle tasks, while the
steps already running there finish, then the step is offered by itself.
Idle slices running on the machine are stopped rather than waited for, as
the builder would stop them for real work itself; they report as yielded and
their lanes run again later.
Its row releases the machine, as does the step ending without one, and every
platform on the machine looks for work again.  A task whose next step also
wants measuring keeps the machine from one step to the next, so a cold task
measures its steps back to back, and the first step that doesn't need it lets
the machine go.  Other tasks on the machine wait where they are, between
steps, bound to their builders, and when the machine frees every platform on
it looks for work again: whichever finds a step first that needs measuring
takes the machine next, there's no queue.

This is the one-time efficiency hit the clean measurements cost: a machine
seeing a task for the first time runs it alone, step by step, before the
other platforms on it get work again.  To keep it from being paid for
nothing:

 - a machine that hasn't gone quiet after 15 minutes, eg, because a long test
   step is running on another of its platforms, runs the step with company
   after all, and isn't asked to go quiet for that step again for an hour

 - a step whose row comes back with company although nothing else was
   offered to the machine, eg, because another sai-builder on it didn't name
   the same `hardware`, or one of its own idle slices was still stopping,
   isn't run alone again for a day; keeping the machine quiet evidently
   doesn't make it quiet

 - a step that never reports a row releases the machine after 3 hours,
   whatever became of its task

The builder list in sai-web shows what the planner is doing with each
machine: the platform measuring a step alone, outlined solid; one waiting for
its machine to go quiet so it can, outlined dashed; and the other platforms
on that machine, held back meanwhile, outlined in amber, each saying which
step they are waiting on.  The task's log says the same from the step's side:

```
planner: step 3: waiting for l2 to go quiet to measure it alone, 2 steps still running
planner: step 3: running alone on l2 to measure it
```

## What's next

The measurements are kept per builder platform, and the `hardware` column
says which platforms share a machine.  That's what the rest needs:

 - the server deciding how many steps a machine runs at once, across all its
   platforms, from what those steps took, instead of each platform's fixed
   `instances`

 - a coarse profile of a step's cpu and memory over its run, so steps whose
   peaks come at different times can be overlapped
