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
|`eff_x1000`|added by sai-server: what each unit of the step's demand got of a core, x1000, its work over its wallclock over its demand, for a step known to use `-j` and given `-j2` or more; 0 for a row that says nothing about how the machine was shared, a step that doesn't use `-j`, a first run, a git step or an idle slice|

The last three are the row's account of itself, see below; the planner
reads `demand` and `load_x100`, and draws the machine's curve from the
rows' cpu and wallclocks together rather than from `eff_x1000`.
`load_x100` comes
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

**The machine's capacity.**  Cpu time can't exceed cores x wallclock, so a
row's cpu over its wallclock is cores the machine delivered to that step at
once, a floor under what it has; rows that ran alongside each other for at
least half of both their lengths add up, since averages summed over a
lesser overlap could come to more than the machine has.  The most seen is
the machine's capacity, and the planner never has to be told it: it's found
by modulating `SAI_PARALLEL`.  A step that uses `-j` may run at any `-j` up
to the most it has run with, and at the rung above that when its biggest
`-j` delivered 10% more cores than the rung below did, so `-j` climbs a rung
at a time while climbing pays, and stops where it doesn't, whether that's
the step running out of parallelism or the machine out of cores.  The rung
above is only taken when nothing running has a finish to lose by it.  A
step never run with `-j2` or more runs the biggest rung within the
machine's capacity, or `-j2` when nothing has been measured on it, to find
out.

**The machine's curve.**  Each row also says the mean load the machine
carried while the step ran, where load is the demand of every step in
flight on every platform of the machine, counted from the server's own
offers and the rows that end them, idle slices included.  A step's demand
is the `-j` it was given if it uses `-j`, else 1.  For a step known to use
`-j` and given `-j2` or more, the row says what each unit of its demand got
of a core, its work over its wallclock over its demand; a step that doesn't
use `-j` says how fast it ran, which is its own affair, and a step's first
run can't tell the two apart, so neither says anything about the machine.
That is the row's own account of itself, and it's kept with the row.  What
the machine's curve is drawn from is the rows together: a row's reference
work over its wallclock is the cores of work the machine got done for it,
and with the rows that ran alongside it, added up as for the capacity, it's
the cores of work the machine got done at once at the load the row ran at.
That and not the cpu burned, because a thrashing machine burns as much cpu
as ever and gets less done with it; the capacity is right to count the cpu,
the curve isn't.  No one step's share says much, a `-j16` step that only
turns a third of its jobs into work shows a third of the machine on its
own, but the sum of what was running does.  Filed by load, rounded to the
nearest of 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48 and 64, each load keeps the
most work the machine was seen getting done at once there, since each sum
is a floor, the rows beside it that hadn't landed yet aren't in it, and the
most of the floors is nearest the truth; that over the load is what each
unit gets there, and load x that is what the machine delivers at the load:
at most its capacity, and less where the rows show the load made it
thrash.  A load with no rows is assumed as good as the nearest load below
it that has some, and a load seen few times a little better than it was: a
tenth better once, less with more.  Rows older than 14 days don't shape the
curve when the server starts.

**How far the load goes.**  The machine carries at most twice its capacity
in demand, so each unit has half a core: the steps on a machine are never
all busy at once, one waits on the network while another waits on the
disk, so a load past the core count gets more done than the core count
would, and twice it is as far as that is taken.  Within that, the curve
says: a load whose rows show the machine delivering under three quarters
of what it delivered at a lower load is thrashing, and the rung below it is
as far as the load goes until rows at the lower loads say otherwise.  A
machine nothing has been measured on carries 4, and no machine is held to
less than that by its capacity alone, since the capacity is a floor a quiet
machine has yet to climb.  The task's log says when a row moves the limit,
and the builder list says what it is.

**Shares.**  Besides its demand, each step in flight has an appetite, the
cores it can use: `-j` x s(`-j`) for a step that uses `-j`, where s is how
much of each unit of `-j` it turns into work, the cores it was seen using
for one that doesn't, and half a core for a checkout until its rows say.
What the machine delivers at its load is shared out in proportion to
appetite, as the kernel shares cores among runnable processes, since a
step's appetite is what it keeps runnable: a `-j16` compile that turns a
third of each job into work has three or four of them runnable on average,
not 16.  No step gets more than its appetite, so either the machine covers
every appetite on it or every step gets the same fraction of its own.
That is what lets the load go well past the core count: a git step waiting
on the network, or a test suite sleeping on a socket, asks for little and
leaves the rest to the compile beside it, and the compile is admitted on
the strength of it.  The server keeps account of what each running step
has had of its share, so it knows how much of its work is left.

**The step's curve.**  The same rows, by the step's `-j`, say how much of
each unit of `-j` the step turns into work: a compile near all of it until
its dependency chains run out, a test suite none.  A step seen using more
than 1.3 cores uses `-j`; one given `-j2` or more that didn't is fixed, and
from then on runs `-j1` and counts 1, so a step that ignores `SAI_PARALLEL`
isn't counted as if it used it.  A step never run with `-j2` or more runs
the default `-j2` to find out.  Only rows taken while the step had a fair share
of the machine count towards this: at a load of no more than twice the
machine's capacity in units, so each had at least half a core, or, whatever
the load, where the step got at least half the most cores it has ever
shown there, since a load of io-bound steps is no contention and the row
says so itself.  A `-j2` run that got a third of a core on a machine at
load 12 says nothing about the step.  And a step found
fixed is only ever given `-j1` again, so nothing could change the verdict:
when its newest run at `-j2` or more is older than 14 days it's given `-j2`
once more, in case it has learned to use it or the run that decided was
unlucky.

**The decision.**  Each `-j` the step could start at is tried.  The shares
of everything on the machine with it added say what the machine would get
done, and the best start is taken if it gets within 2% of waiting; among
starts that tie the smaller `-j` wins, so nothing is spent on no gain,
except the probe rung above when nothing running has a finish to lose.  A
step's work over its share is how long it will take, and the builder stops
a step at its limit, which it tells the server with its platforms
(`step_timeout_secs`, 30 minutes when it doesn't say): a `-j` at which the
step wouldn't be done in half the limit isn't scored, unless the step
couldn't manage that even alone, when it's held to the limit itself; a step
that would overrun the limit alone fails wherever it runs, and isn't held
at all.  Nor is a `-j` scored at which a step already running, with what it
has left to do at the share it would have left, couldn't finish inside
three quarters of its limit: what a step was given when it started is kept
when others arrive.  Nor is a `-j` scored at which a build step that counts
1, this one or one running, would have less than seven tenths of the cores
it can use: it can't make that up with `-j`, it just takes that much
longer, and what it costs the compiles beside it is small.  A step's first
run here, whose work isn't known, isn't started on less than a core, or
half the machine's capacity when that's less, since it would only time
out.  The prediction is logged with the
start, and quoted if the builder times the step out anyway.  A machine
with nothing measured on it carries a load of 4 until a row lands, and a
machine that ran out of memory or timed steps out at a load isn't put there
again, see below.

Nothing is ever run alone to be measured, and no machine is held: a step
that waits does so because every way of starting it would get less done,
it wouldn't finish, or something running wouldn't, and it is offered again
when a step on the machine ends.  A new task's git steps count 1 and are
admitted while that takes no running step's finish.  Idle slices are load
on the machine and share its cores like anything else, but they fill time
real work doesn't want and give way to it: when a real step starting would
get less than it can use with the slices on the cores, one is told to stop,
as the builder would stop it for real work itself.  An idle slice asking to
start needs a whole core nothing wants, so idle can't push out idle.

The task's log says what was decided for each step, in those terms:

```
planner: step 3: -j4 (uses -j), hw2 load 2 -> 6, gets 3.8 of 7.9 cores, 1:12min
planner: step 4: -j1 (doesn't use -j), hw2 load 6 -> 7, gets 0.9 of 7.9 cores, 41.2s
planner: step 4: -j6 (uses -j), hw2 load 0 -> 6, gets 5.1 of 5.1 cores, 58.0s to see if more -j pays
planner: step 3: waits for hw2: at load 23 it would get 0.3 of 7.9 cores at -j1 and take 35:22min, the builder stops it at 30:00min
planner: step 3: waits for hw2: starting this at -j4 would leave a step that has run for 9:10min finishing at 26:40min of the 30:00min its builder allows
planner: step 3: waits for hw2: load 6 is getting 7.6 of 7.9 cores, starting this at -j2 would make it 7.1
planner: step 3: waits for hw2: at load 14 it would get 0.5 of the 1.0 cores it can use, and can't use -j to make that up
planner: step 4: waits for hw2: starting this at -j8 would leave a step that counts 1 with 0.4 of the 0.9 cores it can use
planner: step 5: from this, hw2 is seen delivering 8.3 cores
planner: step 5: from this, hw2 is allowed up to 17
planner: step 4: from this, hw2 is seen delivering 5.1 cores at load 24, against 8.2 at load 12: allowing up to 16 from now
```

The builder list in sai-web says against each platform what its machine is
carrying and delivering, eg, `hw2: load 6 of up to 17, 7.6 of 7.9 cores`,
and when a step on the machine is waiting for the load to drop, the
platforms on it say so.

The memory estimate the builder admits a step on is what the step took at
the chosen `-j` when that's known, else what it took at the nearest lower
`-j` scaled up by the increase, since a build's compilers mostly come one
per job, or what it took at a higher `-j`, which is a safe ceiling.  A step
with no rows here at all, or a task none of whose steps have run here, is
counted as the most any step ever took on its machine until it has: left at
nothing, any number of them could start at once, and when a push starts a
dozen tasks together they do.  The builder adds the estimates of the steps
it has running and admits another only while they all fit in what was free
when it started, peaks counted as if they come together, since they do; a
step it has no estimate for at all counts as an eighth of that.  Whatever
the estimates add up to, it won't take a step the machine doesn't have the
memory for right now, with an eighth to spare, so steps already running past
their estimates stop new ones rather than the OOM killer stopping them all.
A builder that refuses the step for its memory, or because it's full, just
ends the offer; the step is offered again later as anything else is.

**When the OOM killer strikes anyway**, the builder sees it in the step's
cgroup and says so: the step is reported killed for memory rather than
failed, and the task goes back in the queue as a new run instead of failing
the push, since it was the plan that ran the machine out, not the code.  The
old run keeps its log, which says what happened.  A step it took with
nothing else on the machine wants more than the machine has, and fails as
any step does, rather than being requeued to die alone again.  Both ends
learn from it.
The builder's reservations added up to more than the machine could hold, so
from then on it admits steps only while they add up to nine tenths of what
they did when it struck, until it's restarted.  And the planner caps the
machine's load a rung below what it carried at the time, until the server
restarts, since whatever the rows say about its cpu, that load ran it out of
memory; the builder list says `(ran out of memory)` against its platforms.
The point is not to see it again: a step the OOM killer took means the plan
was bad.

**A step the builder times out** is the task's failure when it hangs, or
when it takes longer than the builder allows with the machine to itself.  But
when other steps shared the machine, and the step has been seen to finish,
fairly loaded, in under a third of what it ran for this time, or it ran for
twice what the plan gave it, it was starved by them: the machine was
thrashing, or there was no cpu left for it, and either way it didn't
deliver what the plan counted on.  That's
the plan's doing in the same way, so the task runs again as a new run, and
the planner caps the machine's load a rung below what it carried, with
`(timed steps out)` in the builder list.  The deadline above should keep it
from happening where the curve is known; the cap is for where it wasn't.  A step that genuinely hangs is
capped down to running alone within a few runs, and fails there.

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
