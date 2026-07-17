# Plan: fiber-scoped OTel thread-context publishing

## Unit of attribution: the fiber

The value we publish to the OS-thread TLS record must equal the **currently-running
fiber's** context. Each fiber has its own `(trace_id, span_id, local_root_span_id)`
because the tracer's active context is fiber-local (`Thread.current[@key]`,
`context_provider.rb`). Threads are covered for free: a thread's straight-line code
runs in its root fiber.

## Coupling: push values, relocate the record

Nobody fetches IDs across the Ruby/C boundary at scheduling time. Ownership is split
by what each side natively knows:

- **Ruby tracer** knows the *values* and when they change → it writes them.
- **C hooks** know *which fiber* is current and *which OS thread* hosts it → they move
  the published record.
- **eBPF reader** just reads the OS-thread TLS slot.

The shared identity is the **fiber** (`Fiber.current` ↔ `rb_fiber_current()`).

### Data model (copy model — recommended)

1. **Per-fiber id triple** — 24 bytes `(trace_id[16], span_id[8], local_root_span_id[8])`,
   stored in the fiber's fiber-local storage, wrapped in a Ruby `TypedData` object so
   GC frees it when the fiber dies (see Teardown). This is the source of truth for
   "what this fiber's context is". (`TypedData`, `rb_fiber_current`, and
   `RUBY_EVENT_FIBER_SWITCH` are all available since ≤ Ruby 2.5, so this mechanism works
   across our entire support range — see Ruby version support.)
2. **Per-OS-thread published record** — one libdatadog record attached to the OS
   thread's TLS slot; what eBPF reads. Written via `ddog_otel_thread_ctx_update`
   (in-place seqlock). Belongs to the *OS thread*, not the Ruby thread.

Invariant: **the published record always holds the current fiber's triple.**

Why copy (update the OS-thread record) instead of swap (point TLS at the fiber's own
record): libdatadog's `attach`/`detach` treat the TLS slot as the *owner* of the
record, which fights per-fiber ownership and risks double-free. Keeping one record per
OS thread and copying 24 bytes on transitions uses `update` exactly as intended, keeps
the record set small and stable (matters under OS-thread reuse and M:N), and sidesteps
ownership tangles. The swap model is the alternative if the 24-byte copy ever shows up
in profiles.

## Ruby version support (2.5 – latest)

| Capability | API | Available | Consequence |
|-----------|-----|-----------|-------------|
| Per-fiber triple storage | `TypedData` + fiber-local | ≤ 2.5 | works everywhere |
| Current fiber identity | `rb_fiber_current()` | ≤ 2.5 | works everywhere |
| Fiber switch hook | `rb_add_event_hook(RUBY_EVENT_FIBER_SWITCH)` | ≤ 2.5 | works everywhere |
| Thread resume/suspend/exit | `rb_internal_thread_add_event_hook` | **3.2+** | see below |
| M:N migration | (only exists with M:N) | **3.3+, opt-in** | see below |

The fiber machinery — the heart of this design — is available on the full 2.5–latest
range. The **only** version gate is the GVL internal-thread event API (3.2+):

- **2.5 – 3.1:** no GVL events. These Rubies are strictly 1:1 (no M:N), so there is no
  migration to handle. What we lose is the RESUMED/SUSPENDED/EXITED-driven zero+free of
  the OS-thread record; teardown must come from a Ruby-level `ensure` (a minimal
  `Thread.new` wrapper) — see Lifecycle. Everything else works.
- **3.2:** GVL events (incl. STARTED/EXITED) exist; still 1:1 by default. Use EXITED for
  teardown.
- **3.3+:** M:N is opt-in; RESUMED/SUSPENDED handle migration and cross-thread reuse.

## Events and actions

| Trigger | Where | Action |
|---------|-------|--------|
| trace/span/root changes | Ruby tracer | write current fiber's triple; if the fiber is running, also `update` the OS-thread record |
| fiber switch | C: `RUBY_EVENT_FIBER_SWITCH` (all Rubies) | read current fiber's triple, `update` the OS-thread record |
| Ruby thread resumes on an OS thread | C: GVL `RESUMED` (3.2+) | `update` the OS-thread record with current fiber's triple |
| Ruby thread suspends | C: GVL `SUSPENDED` (3.2+) | zero the OS-thread record ("no trace") |
| Ruby thread exits | C: GVL `EXITED` (3.2+) / Ruby ensure fallback (≤3.1) | zero (and free where safe — see Lifecycle) |

Tracer write path (single internal publisher, called from `Context#set_active_trace!`
and `TraceOperation#activate_span!`/`deactivate_span!`; `deactivate_span!` recomputes
from `trace.active_span`, which walks to the nearest unfinished ancestor):

```
publish(trace):
  if trace.nil?: triple = zeros
  else:          triple = (trace.id, trace.active_span&.id||0, trace.root_span&.id||0)
  store triple in Fiber.current's fiber-local slot
  OTelThreadContext.update(triple)   # updates the currently-attached OS-thread record
```

All-zero `trace_id` = "no trace". No nil→0 coercion inside the native call; the caller
decides.

## Registration of C hooks

- Fiber switch: `rb_add_event_hook(on_fiber_switch, RUBY_EVENT_FIBER_SWITCH, data)`
  (or `rb_thread_add_event_hook` to scope per-thread). Same C event-hook API the
  profiler already uses for allocations (`rb_add_event_hook2` +
  `RUBY_EVENT_HOOK_FLAG_RAW_ARG`, `collectors_cpu_and_wall_time_worker.c:869`). No Ruby
  `TracePoint` object.
- GVL events (3.2+): `rb_internal_thread_add_event_hook` for RESUMED/SUSPENDED/EXITED.
- Spike: confirm `FIBER_SWITCH` fires with the switched-*to* fiber already current, so
  reading its fiber-local triple in the hook is correct. The hook runs GVL-held in
  normal VM context (not the scheduler lock), so fiber-local reads are safe there.

## Lifecycle & teardown (the pre-3.3 reuse edge case)

**Ruby ≤ 3.2 reused OS threads.** A dead Ruby thread's OS thread lingered ~3s in a
pthread cache and could be handed to a *new* Ruby thread (see the removed
`register_cached_thread_and_wait`). Combined with "libdatadog never auto-frees on
OS-thread exit" (the TLS slot is a bare `__thread` pointer, no destructor), this forces
two rules:

1. **Never trust the slot on entry.** A reused OS thread may still hold the previous
   Ruby thread's record contents. Always `update`/zero the record when a thread
   resumes or a fiber switches in — do not assume an empty/own slot.
2. **Zero on the way out.** On SUSPENDED/EXITED, zero the record so an idle cached OS
   thread doesn't expose a dead thread's trace to a sampler during the reuse window.

Freeing (vs zeroing):
- **1:1 modes (all ≤ 3.2; 3.3+ with M:N off):** Ruby-thread lifetime ≈ OS-thread
  lifetime (modulo the cache). Free the OS-thread record on EXITED (3.2) or in the Ruby
  `ensure` wrapper (≤ 3.1).
- **M:N (3.3+):** OS threads are shared and outlive Ruby threads. Do **not** free per
  Ruby-thread; just zero on SUSPENDED and overwrite on RESUMED. The OS-thread record's
  lifetime is the OS thread's; the M:N pool is small and bounded, so the residual is
  negligible. (No clean hook for OS-thread exit; accept bounded retention.)
- **Per-fiber triples:** lifetime = fiber, freed by GC via the `TypedData` free
  callback when the fiber is collected. Uniform across all versions/modes (works on
  2.5+).

**Fork:** child re-derives context lazily (`context_provider.rb:33-36`); the native
TLS slot must be reset in the child (zero/detach) so it doesn't inherit the parent's
record. Confirm behavior and add an `at_fork` reset if needed.

## M:N / Ractor scope

- v1 targets correctness on 1:1 (all ≤ 3.2; 3.3+ default main Ractor). The
  RESUMED/SUSPENDED handling above makes M:N *safe* (no wrong data) and mostly correct.
- Non-main Ractors are M:N by default; ensure the record is per-OS-thread there too, or
  gate them out initially. Reuse profiler detection (`ddtrace_rb_ractor_main_p`,
  `self_test_mn_enabled`) if we choose to gate.

## Performance

- Tracer write path: store 24-byte triple + one in-place `update`. Pack ids in C
  (`rb_integer_pack`). Guard behind a cheap enabled flag → zero cost when disabled.
- Fiber switch / GVL hook: read triple + one `update`. `FIBER_SWITCH` fires only on
  switches (not per-line/call), so cost scales with switch frequency.
- Enabling any global event hook turns on the VM's event dispatch — measure the
  baseline impact of having the fiber-switch hook installed.

## Testing

- Round-trip: tracer publish → `debug_peek` shows integer trace/span, hex root span;
  zeros when no trace.
- Nesting / manual (no-block) traces / continue_trace! / OTel bridge / exception paths
  track `active_span`.
- Fibers: two fibers with different active spans interleave; after each switch the
  record matches the running fiber. Enumerator/`async`.
- **Thread reuse (≤ 3.2):** spawn many short-lived threads sequentially; assert no stale
  record survives into a reusing thread and no record leak.
- **M:N (3.3+, `RUBY_MN_THREADS=1`):** many Ruby threads over few OS threads; record
  matches the running thread's fiber after migration; SUSPENDED zeroes.
- Teardown: no leak after fibers/threads die.
- Verify earliest (2.5) + latest supported Ruby (Matrixfile). Linux only.

## Open questions

1. `FIBER_SWITCH` timing (switched-to fiber current at hook time?) — spike.
2. ≤ 3.1 teardown: accept the minimal Ruby `ensure` wrapper, or drop pre-3.2 support for
   this feature?
3. M:N OS-thread-record retention: accept bounded leak, or find an OS-thread-exit hook?
4. Fork reset mechanism and where it lives.
5. Config surface (env var / setting) and default (off).
