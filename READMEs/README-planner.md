# The planner

sai-server decides how each step of a task runs on a builder from what the
same step took there before.  Nothing about a builder's capacity is
configured: how many cores it really has, how many are left when a VM has
some, whether a step even uses `SAI_PARALLEL`, and how much memory it wants
at each `-j`, all differ per step and per builder platform, and all show in
the measurements.  So sai measures, and decides from the measurements.

Today the planner decides, each time a step could start on a machine,
whether it starts now and with what `SAI_PARALLEL`, from what the machine
gets done at each load and how the step uses `-j`; and which task each
platform takes next across every open event, saying why and when each event
should finish.  The builder's `instances` still caps how many tasks it runs
at once, but the planner usually stops offering before that.

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
|`demand`|added by sai-server: what the step counted as on its machine, its `-j` if it uses `-j`, else 1|
|`load_x100`|added by sai-server: the mean demand the machine carried while the step ran, x100, or -1 if it couldn't say|
|`eff_x1000`|added by sai-server: how fast the machine ran the step, x1000: its work over its wallclock, over the cores it can use; 0 for a row that says nothing about the machine, a git step or an idle slice|

The last three are what the planner reads, see below.  `load_x100` comes
from the server's own account of what it had running on the machine, not
from the builder: `cotenants` is a peak, and a long step's company changes
while it runs.

Rows from a builder that hasn't been updated yet arrive in the old shape,
identifying their step by its index, and are keyed from their task as they
arrive, as the rows from before the change were when sai-server started.

Keying on the step's text rather than its place in the build means editing
the build to add or remove a step leaves the other steps' history where it
is, and keying without the ref means a new branch starts with its task's
history from the branches before it.

The newest 8 rows are kept for each key, step and `SAI_PARALLEL`, so every
point on a step's `-j` curve is kept, however long ago it was measured.  The
newest row from a run alone is kept as well, whatever its age.

## How much to run at once

Two things are decided together whenever a step could start on a machine:
whether to start it now at all, and what `-j` to give it.  Both are judged
by one measure, how much work the machine gets done per unit of real time,
and the planner is after the most of that, not an explanation of it.  It
never has to be told how many cores a machine has, how many a VM really
gets, or which steps use `-j`: those show in the rows, and in no other way.

**A step's work** is its reference cpu time, the least cpu it has ever been
done in on that platform.  Using the cpu a run actually burned would credit
a thrashing machine with more work for the same build; the reference is the
same whatever the run.  A step's first row stands in for its reference until
a smaller one comes.

**The machine's curve.**  Each row says the mean load the machine carried
while the step ran, where load is the demand of every step in flight on
every platform of the machine, counted from the server's own offers and the
rows that end them, idle slices included.  A step's demand is the `-j` it was
given if it uses `-j`, else 1.  The row also says how fast the machine ran
the step: for a step known to use `-j`, its work over its wallclock over its
demand, what it got of what it asked for; for one known not to, how fast it
ran against the fastest it has run.  A step whose earlier rows can't say
which it is, its first run above all, says nothing about the machine, since
that run alone can't tell a step that doesn't want its `-j` from a machine
that couldn't give it.  Filed by load, rounded to the nearest of 1, 2, 3, 4, 6, 8, 12, 16,
24, 32, 48 and 64, the rows give the machine's efficiency at each load, and
load x efficiency is the work rate it gets done at that load.  The load with
the best rate is where its knee is: a 2-core machine's rate is flat from 2
up, a 16-core machine's climbs to 16 or a little past it.  The planner lets
the load go one rung above the best known, so the rung above is measured too
and the knee is found from both sides; a higher load has to beat a lower
one's rate by 5% to become the best, so a flat top isn't climbed on noise.
A machine with no rows filed by load is allowed a load of 4, or, when its
older rows show a step still getting most of its `-j` with company at a
higher load, that load, since it demonstrably delivered it.  Rows older than
14 days don't shape the curve when the server starts.

**The step's curve.**  The same rows, by the step's `-j`, say how much of
each unit of `-j` the step turns into work: a compile near all of it until
its dependency chains run out, a test suite none.  A step seen using more
than 1.3 cores uses `-j`; one given `-j2` or more that didn't is fixed, and
from then on runs `-j1` and counts 1.  A step never run with `-j2` or more
runs the default `-j2` to find out.

**The decision.**  With the machine at load L, waiting keeps a work rate of
L x eff(L).  Starting the step with demand d makes it (L + d x s(d)) x
eff(L + d), where s(d) is how much of `-j`d the step itself turns into work,
relative to its best `-j`; a fixed step is counted as the cores it was seen
using.  Every `-j` that fits under the machine's ceiling is scored, the best
start is taken if it gets within 2% of waiting, and among starts that tie
the smaller `-j` wins, so nothing is spent on no gain.  A load no row has
been filed at is assumed as good as the nearest load below it that has rows,
and a load with few rows a little better than they say, which is what gets
the rung above the best tried.  A machine with nothing running always starts
something.

Nothing is ever run alone to be measured, and no machine is held: a step
that waits does so because every way of starting it would get less done,
and it is offered again when a step on the machine ends.  A new task's git
steps count 1 and are admitted while that fits under the ceiling.  Idle
slices are load on the machine, and the rows filed while they run say so,
but real work doesn't count them when deciding: they fill time real work
doesn't want and give way to it.  When a real step starting would put the
machine over its ceiling with the slices counted, one is told to stop, as
the builder would stop it for real work itself.  An idle slice asking to
start counts everything, so idle can't push out idle.

The task's log says what was decided for each step, in those terms:

```
planner: step 3: -j4 (uses -j), hw2 load 2 of up to 8 -> 6, doing 1.9 cores of work -> 5.6
planner: step 4: -j1 (doesn't use -j), hw2 load 6 of up to 8 -> 7, doing 5.6 cores of work -> 5.9
planner: step 3: waits for hw2: load 8 of up to 8, nothing more fits
planner: step 3: waits for hw2: load 6 of up to 8 is doing 5.6 cores of work, starting this at -j2 would make it 5.4
planner: step 5: from this, hw2 gets most done at load 8, allowing up to 12
```

The builder list in sai-web says against each platform what its machine is
carrying and how far the planner lets it go, eg, `hw2: load 6 of up to 8`,
and when a step on the machine is waiting for the load to drop, the
platforms on it say so.

The memory estimate the builder admits a step on is what the step took at
the chosen `-j` when that's known, else what it took at the nearest lower
`-j` scaled up by the increase, since a build's compilers mostly come one
per job, or what it took at a higher `-j`, which is a safe ceiling.  A
builder that refuses the step for its memory, or because it's full, just
ends the offer; the step is offered again later as anything else is.

## What runs next

Each builder platform runs one task at a time per instance, and asks
sai-server for another when one finishes.  Which of the tasks pending for the
platform it gets, across every open event, is decided by the plan, which
sai-server recomputes when tasks change state, rows land or builders come and
go, coalesced, and at least once a minute while anything runs.

The plan starts from how long each unfinished task should take on its
platform: the newest row of each of its steps there added up, as the builder
is told when offered the task.  A task with no rows yet is guessed from its
neighbours: the mean of the tasks with history on the same platform, counting
what the open events' finished tasks took there, else across every platform,
else 5 minutes; the plan marks it a guess whichever it got.  A task part way
through counts only the steps it has left, and a running one what should be
left of it; one past its estimate is given a tenth of what it has taken so
far.  From
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
offered before the plan is consulted, as before.  Whether a step actually
starts when it comes to the top, and at what `-j`, is the machine's decision
above, and the plan says against each platform what its machine is carrying.
Idle tasks are not planned: they run when nothing else will, and yield when
something will.

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
to take, a builder connecting or going away, and a step on a machine ending.
The estimates behind it are otherwise kept for an hour.

## What's next

 - a coarse profile of a step's cpu and memory over its run, so steps whose
   peaks come at different times can be overlapped
