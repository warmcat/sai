# The planner

sai-server decides how each step of a task runs on a builder from what the
same step took there before.  Nothing about a builder's capacity is
configured: how many cores it really has, how many are left when a VM has
some, whether a step even uses `SAI_PARALLEL`, and how much memory it wants
at each `-j`, all differ per step and per builder platform, and all show in
the measurements.  So sai measures, and decides from the measurements.

This is the start of it.  Today the planner chooses each step's
`SAI_PARALLEL`, which was always 2, has a step measured alone on its machine
when nothing comparable has been, and decides which task each platform takes
next across every open event, saying why and when each event should finish;
the builder still decides for itself how many tasks it runs at once.

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

**Exploring is switched off** (`sais_plan_explore` in `s-planner.c`): a step
runs at the best `-j` its rows on the platform show, `-j2` when it has none,
and no machine is held to measure anything.  Climbing each step's own ladder
a rung per run, with the first run at each rung alone, had a cold builder with
40 tasks running every step of every task one at a time, and what it learned
about one step said nothing about the next.  The sections below describe the
ladder and the holds as built; what replaces them learns the machine's
throughput under load from the rows every step sends anyway.

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

## What runs next

Each builder platform runs one task at a time per instance, and asks
sai-server for another when one finishes.  Which of the tasks pending for the
platform it gets, across every open event, is decided by the plan, which
sai-server recomputes when tasks change state, rows land or builders come and
go, coalesced, and at least once a minute while anything runs.

The plan starts from how long each unfinished task should take on its
platform: the newest row of each of its steps there added up, as the builder
is told when offered the task, and a flat 5 minutes for a task with no rows
yet, which the plan marks as a guess.  A task part way through counts only
the steps it has left, and a running one what should be left of it.  From
that each open event has a known amount left on each platform, and since an
event is finished when its slowest platform is, the platform it will finish
last on is its bottleneck and the time left there its critical path.  That
is also the event's ETA.

A platform's queue is then made in this order:

 - **the event with the shortest critical path first.**  It's the push that
   can be finished soonest, so the most pushes get their result soonest, and
   a small push on one repo isn't held up by a big one on another.  This
   replaces the repos taking turns on a platform, which did the same job
   with less to go on.

 - **a task of a later event goes ahead only where it fits.**  An event whose
   finish waits on some other platform has that long to spare here, and a
   task that runs here first in that time doesn't delay it, so the slack of
   every event ahead is spent on tasks that fit in it.  Spending it brings
   the later event's finish forward for nothing.

 - **within an event:** a task this builder already started and is waiting
   to continue, since its earlier steps' work is in the job dir; then the
   tasks that failed on this branch last time, so a regression shows as early
   as it can; then the longest first when the platform runs several at once,
   else in task order.

 - **the head stays put.**  The event at the head of a platform's queue stays
   there until another's critical path beats it by 10%, so the head doesn't
   change between events that are close every time a row lands.

A task this builder already started and that is waiting for its next step is
offered before the plan is consulted, as before.  Nothing new is offered to
a platform whose machine is held for a measurement, see above, and the plan
says so against it.  Idle tasks are not planned: they run when nothing else
will, and yield when something will.

How many tasks a platform runs at once is what its builder's `instances`
says, 6 when it isn't configured, which a builder new enough sends with its
platforms; for an older one it's the most the server has seen running there
since it started.  Several
builders serving one platform add up.  The ETAs assume that: a platform's
queue drains that many at a time, each task taking what it took before.

sai-web shows the plan in two places.  Under each builder in the builder list
is what its platform will get next: up to three tasks with the time each
should start and take, a `?` on a guess, the reason as the row's tooltip,
and a line with how many are queued and when the queue should be done, whose
tooltip is the policy above in a sentence.  Against each event, in the
sidebar and in its header, is when the planner expects it to finish and, as
the tooltip, how many tasks it has left, how many are running and how many
are guesses; an event waiting on a platform nothing serves says that instead
of a time.  None of it ticks: it changes when a new plan arrives, which is
at most every 5 seconds.

The reason a task was chosen goes in its log when it's offered, eg

```
planner: libwebsockets 5f39aa48: shortest to finish, 12m left, waiting on freertos-espidf/xl6-esp32/gcc; failed last time on this branch
planner: fits ahead of libwebsockets 0c5b0d5f, which waits on rocky9/x86_64-amd/gcc for 25m anyway; longest first, 2 run at once
```

What makes a plan change: a task starting, finishing or failing, a metrics
row landing for a task in it, which also replaces what that task is expected
to take, a builder connecting or going away, and a machine being held or
released for a measurement.  The estimates behind it are otherwise kept for
an hour.

## What's next

The measurements are kept per builder platform, and the `hardware` column
says which platforms share a machine.  That's what the rest needs:

 - the server deciding how many steps a machine runs at once, across all its
   platforms, from what those steps took, instead of each platform's fixed
   `instances`

 - a coarse profile of a step's cpu and memory over its run, so steps whose
   peaks come at different times can be overlapped
