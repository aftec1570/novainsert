# RT-safety audit (F1 §14; plan Task 26 Step 1)

Automated proof: `tests/unit/rt_audit_test.cpp` – `rt_audit_full_session_allocates_nothing_on_rt_entry_points` drives
10 000 device cycles (B = 64) through the real `HookDispatcher::onAudioBuffer` trampoline and the processBlock path
(`Instance::process`, `MeterEngine::feed`, `SlotLoopEngine::cutProbe`) across idle, play, a probe measurement, the measured
lock (PDC change), a seek, bypass toggles, trim/selector changes, a record cut and a stop, counting allocations only
inside the RT calls → 0. It runs in all four `tests/run_unit.sh` presets (debug, release, ASan+UBSan, TSan).
Sensitivity: an allocation injected into `Instance::process` makes it fail (`rtAllocs == 0` FAILED, checked 2026-10-30);
`rt_audit_alloc_counter_sees_an_allocation` keeps the counter honest.
Limitation: `AllocGuard` intercepts global `operator new` only (not malloc/calloc/realloc, not aligned new); the code
inventory below covers those by inspection. Deadline/runtime measurement is F5 (F1 §14), not this audit.

## RT entry points

| entry point | thread | does | must not |
|---|---|---|---|
| `HookDispatcher::onAudioBuffer(isPost, len, srate, reg)` (static) | REAPER device thread | span/dropout clock, null-buffer streak, BufferChange detect, gathers ≤ 128 channel pointers via `reg->GetBuffer`, fans out to ≤ 20 client slots | allocate, lock, log, call any other host API |
| `Instance::onPre` / `onPost` → `SlotLoopEngine::onPre/onPost` | device thread | double↔float channel selection, return ring push / send ring pop, probe send + capture, fence | touch processBlock state except through the rings and the segment seqlock |
| `NovaInsertProcessor::processBlock` → `Instance::process` → `SlotLoopEngine::process` | host processBlock thread | play head read, record cut, meter feeds, send ring push / return ring pop, segment anchor, fades | allocate, lock, log |
| `MeterEngine::feed` | processBlock thread | peak/RMS/VU/clip ballistics, publishes atomics once per call | allocate |
| `SlotLoopEngine::cutProbe`, `setRouting`, `setMuted`, `setFenced`, `setProbe`, `setLatency` | any (atomics) | publish control words | block |
| `DiagLog::row` | any | fixed-size record into the MPSC ring; drops counted | format, IO (the writer thread formats) |

Not on the RT path (message thread or worker only): `Measurer::tick`, `CorrelatorWorker` (owns a `std::mutex` and a
`std::thread` – never touched from RT; jobs are submitted from the message thread), `PdcReporter`, `StateMachine::apply`,
`RenderArbiter`, `Library`, `persistence`, the editor. `StateMachine::state()`/`flags()` are atomic reads, RT-callable.

## Queues

| queue | type / capacity | producer → consumer | overflow rule | memory order | lifecycle |
|---|---|---|---|---|---|
| send ring | `Spsc<Frame, 2·kLMax>` (131 072 frames) | `process()` → `onPost()` | counted in `Status::sendOverflows` | acquire/release head/tail | allocated in `prepare()` (message thread, no RT running); never reset – a new segment flushes by index |
| return ring | `Spsc<RFrame, 2·kLMax>` | `onPre()` → `process()` | dropped while nobody consumes (stopped); `process()` flushes at every segment start | acquire/release | as above |
| segment snapshot | sequence lock | `process()` → `onPost()` | reader retries next cycle on an odd/changed seq | release writer, acquire reader | member of the engine |
| hook events | `Mpsc<Reason, 256>` | hook → message thread (`drainEvents`) | `eventOverflow_` counted | per-cell seq acquire/release | member of the dispatcher |
| diagnostics | `Mpsc<DiagEvent, 4096>` | any thread → writer thread (20 ms) | `dropped()` counted | per-cell seq | process lifetime singleton |
| probe capture | `float[4·kLMax]` ×2 | hook writes by index → worker reads after `capturedCount()` publishes | bounded by `kCaptureCap`, extra samples ignored | release count / acquire | allocated in `prepare()` |
| hook client slots | `std::atomic<IHookClient*>[20]` | message thread → hook | `registerClient` returns −1 when full | acquire/release; `unregisterClient` waits for the in-flight cycle on the message thread | dispatcher lifetime |

## Host calls made from RT, with F1 §2 evidence

| call | from | evidence |
|---|---|---|
| `audio_hook_register_t::GetBuffer(isOutput, idx)` | hook | F1 §2 row `Audio_RegHardwareHook` – F0.2 (144+36+36 cases), F0.4, F0.6; `len == BSIZE`; `input_nch/output_nch == 0` on 7.83 so counts come from `GetNumAudioInputs/Outputs()` read on the message thread |
| `juce::AudioPlayHead::getPosition()` (VST3 `ProcessContext` copy, no host round trip) | processBlock | JUCE 8.0.15 VST3 wrapper; play/record flags proven in F0.2–F0.6 (`GetPlayState`, record detection row) |
| `isNonRealtime()` (JUCE flag set by the wrapper) | processBlock | F1 §2 row `IsInRealTimeAudio` – non-RT exclusion, F0.2–F0.6 |
| `setLatencySamples` | **never from RT** – message thread only | F1 §2 last row, F0.3 |
No other REAPER API is called on the hook or processBlock path (inspection of `hook_dispatcher.cpp`, `instance.cpp`,
`slot_loop.hpp`, `meters.hpp`, `processor.cpp::processBlock`, 2026-10-30).

## Deferred to the bench (plan Task 26 Steps 2–4)
`f3_11` fault driver (14 scenarios × 4 buffers × 3), `long_run_10min` bounded-queue check (diagnostics/event overflow
counters 0 under 20 instances), Instruments Leaks smoke – Apollo-only, after 2026-11-01.
