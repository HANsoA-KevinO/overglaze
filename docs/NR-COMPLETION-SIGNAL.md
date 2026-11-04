# Live submission metadata and completion publication

The shared live NR router owns one enrolled game recording. Native lifecycle
observations and actual same-queue submission remain distinct from private
fence completion, pipeline retirement and release of borrowed resources.

## Observed failures

The first PBZ failure named `timeline-lock-contention` after 64 attempts to
publish a post-Execute Signal. Retaining that task instead of invalidating the
timeline passed controlled GPU tests and a bounded game check, including two
real lock misses followed by continued execution.

A later process still stopped after 346,120 Evaluate calls and retirements, with
no tracked NR work in flight. Its first saved error named
`submission-observer-fault; submission=reset-lock-contention`. A single missed
try-lock of the router metadata mutex had permanently faulted the Reset
observer. Close and both sides of Execute had the same policy. The first saved
failure timestamp is not an independently recorded native Reset timestamp; the
lock holder and exact cutscene frame were not captured. The earlier short check
does not establish extended stability.

## Metadata synchronization

- Reset, Close and Execute entry/return must preserve observations. They
  serialize through a short router metadata critical section instead of making
  lock contention fatal. Wait counters distinguish each lifecycle phase.
- Recheck the watched list after acquiring synchronization. A worker may have
  retired the old recording and enrolled another one while a callback waited.
  Post-Reset and post-Execute observations also verify the held list and lease
  serial before publishing their result.
- Native Reset/Close/Execute, queue Signal, timeline inspection and COM releases
  occur outside the router metadata mutex. Copying a retained native queue
  reference under the mutex is an ownership operation. JSON allocation occurs
  after metadata has been copied. In particular, no router holder waits for a
  queue submission scope or the timeline mutex.
- The serial owner also preserves arm/retirement metadata operations across
  reader contention. It inspects the timeline outside the router mutex, then
  revalidates the lease before releasing it. Pre-recording enrollment may still
  report busy and safely bypass the frame before injecting any GPU work.

This trades a lost mandatory event for serialized CPU metadata access. It is
not a GPU drain, a fixed retry allowance, or a guarantee of a maximum OS thread
scheduling delay.

## Retained completion tasks

- After the original Execute returns, retain a Signal task tied to the verified
  queue and ticket. The fast path attempts it immediately; a busy timeline keeps
  the task and lease for a later serial-owner poll.
- For an armed recording Reset before any submission, retain the old lease and
  detach its pointer watch before calling native Reset. Only a successful Reset
  proves destruction of the old recording. Publish that discard proof after the
  native call; timeline contention defers it without losing the proof.
- One atomic publisher serializes both task kinds across callback and owner.
  The owner must confirm publication before retiring the frame, so it cannot
  cross the timeline/router acknowledgement gap. Reset of a submitted list
  starts a new recording and cannot discard the old pending Signal.
- A short owner-side grace can avoid an unnecessary bypass; expiry leaves the
  task pending rather than invalidating it. Repeated polls cannot send another
  Signal or acknowledge a discard twice.
- Genuine queue/state disagreement, invalidation, failed native Reset/Close,
  repeated submission and failed Signal remain faults. Unknown GPU dependencies
  stay retained. Actual fence completion is still required for submitted work;
  a proven pre-submit discard never claims GPU completion.

## Regression scope

Controlled metadata holders cover Reset, Close, Execute entry and Execute
return independently. Additional cases cross a paused old Reset with real
retirement and a new enrollment, run lifecycle/owner operations with concurrent
metadata readers, and hold the timeline while preserving successful Reset
proof. Six pre-repair cases fail at their expected assertions; the repaired
cases require correct observation and ownership, not merely absence of a crash.
Existing queue, duplicate, unarmed Reset, discard and delayed Signal tests remain.

Test access is compiled into a separate fixture object only; the shipping
router has no test callbacks. Existing present-queue/overlay interoperability
and real-model default/binding-preservation fixtures exercise shipping objects.
The exported Live ABI remains 27. After deployment, PBZ loaded the
new host and bridge, and the user reported normal NR through a cutscene and
menu/load transitions. Same-process samples increased from 1,039/1,038
Evaluate/retirement calls to 4,004/4,003, with one frame in flight and no error
or runtime/admission skip. The earlier two history gaps and one initialization
network rebuild did not increase. Public status does not expose lifecycle
metadata waits, so this game check does not prove those contention branches
were exercised. It is a short functional check; cross-game quality, performance
and extended stability remain unverified.
