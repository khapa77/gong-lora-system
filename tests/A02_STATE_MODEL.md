# A02.1: persistence contract and host verification

The A02.1 core is implemented in the working tree; this document describes its
accepted contract, not a firmware/hardware qualification. The current scheduler is
included directly by `test_active_day_state.cpp`; its function bodies are not
extracted, rewritten, or replaced. ArduinoJson uses the already installed
headers. The runner never invokes PlatformIO, installs packages, or opens a
physical device.

## Run

From the repository root, supply an approved scratch directory explicitly:

    python3 -B tests/run_active_day_state_tests.py --build-dir <scratch-directory>

Use `--case regression/power_cut_pair` for a tight reproduction or `--group
control` to check existing behavior. The C++ fixture is compiled with C++11,
`-Wall -Wextra -Werror`. Each case runs in a separate process. The runner writes
its binary and `results.json` only to the requested directory outside the repo.
A nonzero result is expected on the unfixed A02 baseline; failures must not be
relabelled as passing tests.

Independent verification modes: `--malloc-fence`, `--sanitize undefined`, and
`--sanitize undefined --malloc-fence`. Bare `--sanitize` selects UBSan, not ASan
and not a C allocation fence. ASan is NOT EXERCISED: the installed host runtime
also terminates a minimal malloc/free program with SIGILL; do not infer an A02
memory defect from that startup failure or repeat its diagnosis for this stage.

## Test groups

- `regression`: safety assertions against current production functions. The
  historical pre-A02 RED demonstrated a mixed legacy pair and extra advancement;
  current cases require a whole old/new pair and no double advancement.
- `control`: existing startup, date advancement, last-day behavior, lastfire,
  and A01 deduplication behavior that must remain intact.
- `contract`: implemented `active` precedence, retained migration inputs,
  invalid-state refusal, and explicit exact-record Recovery followed by Resume.
- `model`: fault-backend self-tests, not production coverage by themselves.
- `T01`–`T12`: wire/date validation, boot precedence, migration, transaction
  faults/cuts, publication/OOM, execution guards, pending-save, Recovery/Resume
  and A01/key-isolation regressions. Review cases extend T07 and T08 with
  initialized tails and post-course-replacement file BLOCKED transitions.

`@state` in a fault specification means the primary active-day write: legacy
`aday` on the historical baseline, or `active` now. The same power-cut assertion
survives the storage-format change without weakening its expected behavior.

## Logical NVS model and limitations

The backend has separate visible and durable maps. A successful modeled set
changes the visible map; successful commit copies it to the durable map. Reboot
discards volatile state and reopens handles against durable data. A failed set
or commit can be injected with either the old or new value durable. Reads can
fail or return an unexpected scalar. A power cut can interrupt before or after
commit. Preferences putInt/putString use separate set/commit pairs, matching the
installed Arduino-ESP32 2.0.16 implementation's boundaries.

This deliberately adversarial logical model does not reproduce NVS flash-page
layout, wear leveling, CRCs, driver caches, or electrical failures. Its selected
old/new outcomes are test scenarios, not claims about the physical result of
any particular ESP-IDF error. Host String and LittleFS are adapters; hardware
sound, RTC, concurrent FreeRTOS execution, and flash durability are not tested.

## Wire format

Namespace: `gong`. Key: `active`. NVS type: uint64.

- Bits 63..56: version, initially 1.
- Bits 55..48: course day, checked against DAY_COUNT.
- Bits 47..32: reserved, must be zero.
- Bits 31..0: YYYYMMDD; zero means date not yet bound.

Encode fields explicitly; do not persist a compiler-dependent struct layout.
Validate a nonzero date as a Gregorian date representable by YYYY-MM-DD and
in years 2024..9999, including Gregorian leap-year rules. Do not interpret a bad
nonempty date as an unbound one.

`active_schema` is a separate uint32 marker equal to 1. It records introduction
of the format, not completion of migration. Before the first active write it
uses set/commit/readback; legacy keys and the legacy file remain read-only.

## Production state machine

Let S be the last published live state and C a fully prepared candidate. States:

- BOOT: no authoritative state established.
- EMPTY: absence of active, legacy day/date, and legacy file was confirmed.
- LOADED: a validated record exists but its schedule has not yet been published.
- READY(S): live schedule, course day, and date agree with an accepted record.
- PREPARED(S,C): candidate parsed/allocated; S is still published.
- SET_ACKED(S,C): nvs_set_u64 returned success; RAM is still S.
- COMMIT_ACKED(S,C): nvs_commit returned success; readback remains outstanding.
- VERIFIED(S,C): readback succeeded and equals the exact encoded candidate.
- UNKNOWN(S): I/O outcome cannot be trusted; retain S only for diagnostics.
- INVALID: unsupported version/type, invalid record or incomplete legacy state.

Persistence status, transaction phase, file state and execution permission are
independent. File states are Unprepared, Ready and BLOCKED. Effective execution
requires persistence READY, file Ready and executionRunning; READY/paused cannot
fire, catch up, advance, save or mutate schedules.

Normal transition:

    READY(S) -> PREPARED(S,C) -> SET_ACKED -> COMMIT_ACKED
             -> VERIFIED -> allocation-free publish -> READY(C)

Failure decisions:

- Candidate preparation failure before NVS, when the accepted live file has not
  been replaced: retain READY(S), reject the request; no automatic promotion of C.
- Any non-success result after attempting set, any commit error, readback error,
  or unexpected readback: latch UNKNOWN. A read of C after a commit error is
  not proof of durable success, and must not silently restore READY.
- While UNKNOWN, INVALID, paused or file BLOCKED: no automatic fire, catch-up, advancement,
  pending-save flush, or timed recovery write. Do not stop an already playing
  track or change manual audio/Wi-Fi/RTC behavior as part of A02.
- Do not erase NVS, auto-reboot, delete keys, or issue a compensating rollback
  write. A rollback would create another ambiguous transaction.
- Loss of power before commit: normal NVS boot recovery may yield a whole S or
  C, or a read error; accept only a validated complete record. The host model
  currently exercises selected outcomes, not the real NVS implementation.
- Loss of power after successful commit but before RAM publication: next boot
  loads the complete durable C and prepares its schedule afresh. It must not
  advance from a mixed day/date pair. Existing calendar advancement rules may
  then run on that complete state.

## Bounded explicit NVS Recovery and Resume

`sched_recover(expected, discardPending=false)` is internal, mutex-serialized and
normally admitted only from UNKNOWN. There is no automatic retry loop. It makes
one attempt:

1. Read and validate the authoritative key and require an exact uint64 match to
   expected. If invalid, mismatched or unreadable, remain
   locked; do not fall back to stale legacy keys.
2. Prepare the schedule for that exact stored day/date, without advancing or
   restamping it.
3. Re-save that exact record, commit and read it back. Merely reopening a handle
   or observing a cached value is not a durability acknowledgement.
4. Publish only after success, READY/paused. Transaction errors remain UNKNOWN;
   invalid authoritative input can be classified INVALID. Never retry automatically.

Pending edits freeze in quarantine. Recovery refuses them without explicit
discardPending; unsuccessful Recovery retains them. Only successful publication
discards pending. Ordinary activation first requires a successful old-day flush.
Date-only transactions preserve live edits and their pending ownership.

Repeated Recovery in READY/paused succeeds only for the published expected
record, without another write. Running Recovery is rejected. `sched_resume()`
requires persistence READY and file Ready, performs no I/O or immediate date
advancement and is idempotent. Pausing is RAM-only: valid normal reboot runs.
UNKNOWN/INVALID cannot be repaired by ordinary activation or file reload.
HTTP/UI/serial recovery belongs to A02.2 and is not implemented.

## File BLOCKED and explicit reload

Boot file/preparation failure keeps the validated record LOADED. After course
replacement, an active-file reload failure returns false with an error and
latches file BLOCKED; persistence stays READY and old RAM is diagnostic only.
Neither Resume, activation, NVS Recovery of a healthy record, automatic ticks,
date advancement, saves, template/live edits nor course mutations clear it.

`sched_load()` is an explicit, mutex-serialized reload of the accepted record's
current `/dayNN.conf`, never the `/gong.conf` seed or an arbitrary fallback.
Missing/unreadable files, JSON preparation failure and OOM keep BLOCKED. Only
successful preparation/publication clears it, retaining the exact day/date and
leaving execution paused until Resume. For an already accepted active record
this does not write NVS; a deferred legacy boot migration still requires the
normal marker/active transaction before publication. Pending edits refuse reload.
There is no new HTTP/UI/serial caller.

Course renames remain sequential, not an atomic multi-file import (A03). File
writes still lack a temp/rename durability protocol (A04); full JSON schema
validation remains A06. A normal pending flush failure retains pending and does
not stop a consistent live day; it is not the post-replacement BLOCKED case.

A partial import also latches BLOCKED if the active file was attempted or had
already been replaced before a later rename failed. Even a failed active-file
rename/remove can leave its contents indeterminate. A failure before touching
the active file keeps the consistent live day running; NVS status is unchanged.

## Migration rules

- Marker read/type/value errors fail closed before reading active. A valid
  active key is authoritative, even when the marker is absent; boot does not
  rewrite it just to add a marker. Only missing marker AND active permits inspection of
  legacy aday/adate. Read errors, wrong types and invalid/unknown active formats
  are not equivalent to absence and do not permit silent fallback.
- Valid legacy day plus a missing/empty date preserves the unbound-date case.
  Orphan adate, invalid day/date or read errors are not first boot.
- If no legacy NVS state exists, inspect /activeday.conf before declaring EMPTY.
- Write and verify active once, then use it. Keep old keys and the legacy file;
  do not dual-write. A failed migration must not activate Day 00 or delete its
  input. Already inconsistent but plausible legacy pairs cannot be repaired
  automatically from the available evidence.
- Once migrated, do not automatically resume legacy fallback after active is
  damaged. Downgrade to old firmware requires an explicit restoration procedure.
- Marker present but active absent is UNKNOWN, never EMPTY or legacy fallback.
  Unavailable FS is not proof that legacy files are absent.

File presence uses PRESENT / CONFIRMED_ABSENT / ERROR, not the boolean result of
LittleFS.exists() or open(). With the installed Arduino-ESP32 2.0.16 / ESP-IDF
4.4.7, VFS stat reports ENOENT separately from I/O/allocation errors. The probe
checks the /littlefs/ mount root and regular-file type; only target ENOENT under
that root confirms absence. This matches main.cpp's LittleFS.begin(false)
default mount; changing that mount requires updating the probe. Legacy lookup
errors and first-boot day/seed access failures latch UNKNOWN without an active
write. Ordinary candidate preparation failure preserves a consistent READY day.
Creation of /dayNN.conf requires confirmed absence; an unreadable present file
must never enter the destructive write/create path. Host stat faults are
independent of missing files and open failures. Legacy JSON dates additionally
compare the full decoded JsonString length with the C-string length, rejecting
embedded NUL rather than migrating a truncated or unbound date.

## Allocation-free publication proof obligation

The implementation separates preparation, persist/verify and
`publishPrepared() noexcept`; host fences exercise that boundary:

- Complete JSON parsing, every String allocation, the date, metadata and encoded
  record before set/commit.
- Value-initialize every ScheduleEntry in PreparedDay, including unused slots.
  Empty/short and long-to-short regressions inspect the entire published tail;
  no old entries or descriptions remain beyond count.
- Publish only scalar assignments and moves of already prepared String storage,
  under the existing state mutex. Do not copy Strings, resize containers, parse,
  serialize, call files/NVS/clocks, log, or invoke callbacks inside publication.
- The installed Arduino 2.0.16 WString.cpp:234-260 and :275-278 implement String
  moves through memmove/free/pointer transfer, with no allocation. Its copy
  assignment is different and must not be used for publication.
- Review the actual publish call graph and enforce an allocation-forbidden fence at
  the verified-to-published boundary. Host adapter coverage alone is not proof
  of target String or allocator behavior; keep the framework-source evidence.

Production host cases reach new-key set/commit/readback failures, mismatches,
Recovery, interrupted migrations and publication. Separate child self-tests
prove malloc/calloc/realloc/new fence rejection with SIGABRT, not just any
nonzero exit. Keep current machine-readable receipts and input hashes separate
from historical RED/GREEN results; none of these qualify ESP32 hardware.
