# Report collection quality

This isolated implementation distinguishes diagnostic content from successfully
writing a file. It does not classify game compatibility or infer a crash.

The report collector renders structured records directly through the privacy
filter. It never falls back to raw stdout/stderr or packages `.records` files.
Filtering covers an entire rendered record, including embedded newlines;
oversized records are omitted conservatively. The existing account, computer,
path and credential filter remains enabled for descriptions and diagnostics.

`report-quality.txt` contains only fixed schema fields, states and counters for
the current and previous logs. Headers and omission notices are not useful
diagnostic content. The collector distinguishes missing, empty, damaged,
size-limited, unrendered, fully filtered, partial, I/O failure and useful content.
Several flags may coexist, for example a useful prefix followed by truncation.

Manual sends require a description after privacy filtering: at least 12 visible
characters and four letters/digits. All existing localized placeholders are
rejected, regardless of the selected interface language. Whitespace and invisible
format characters do not satisfy the threshold. Validation occurs in the dialog
and the package builder; a failed dialog validation preserves the user's text.

The user can save an incomplete draft locally. Draft packages are marked
`draft_incomplete` and `sendable=0`. A report with no useful diagnostics can be
sent manually with a useful description and an explicit warning, after inspecting
the quality of the actual final package. The warning defaults to No. The package
is labelled `diagnostics_unavailable` or `diagnostics_partial`, as appropriate.
Opening, validating, cancelling or saving the dialog does not send a report.
The existing optional automatic setting is not enabled or rewritten; its path
now refuses incomplete diagnostics instead of silently uploading empty notices.

## Optional record writer

The core still opts in only through `PS5C_LAUNCHER_RECORDS`. Its additional
stream is unbuffered, so process termination cannot discard pending CRT buffers.
Critical records additionally check a stream flush. This guarantees neither
recovery from external termination during a write nor durability against power
loss. There is no global flush and no new teardown wait for a logging lock.

A try-lock serializes optional writes without waiting for a faulting or suspended
writer. Contended records are dropped and counted; a subsequent write or normal
finish persists that count. If termination interrupts the writer before that
update, the collection remains marked partial. Raw diagnostic behavior is
unchanged. The optional channel stops on a write/flush error or its 64 MiB limit;
serialization rejects are counted without retaining their values or formats.

`<game>.log.records.status` uses the bounded `ZRS1` numeric format for open,
write, flush and setup errors, serialization rejects, limit, contention and normal
completion. An interrupted/invalid status file is treated as damaged metadata.
Older records without this sidecar remain readable with `status_known=0`; their
usefulness does not authenticate the version or completeness of an old session.
New sessions use immutable directories under user/logs/<game>.sessions/<id>.
Folder-name fallbacks with spaces/non-Latin text and reserved device names use a
bounded filename key; they do not require an official title ID to launch.
The launcher writes an opaque 32-hex-character identity into both raw and
structured streams. The index publishes current/previous/pending pointers by
one checked atomic replacement; files from different runs are never renamed
independently. Failed attempts remain explicitly pending/unavailable while the
last ended session remains bound to its own identity. Per-game failure markers prevent silently falling back after an index/metadata write failure.
A per-game exclusive lease spans launch, execution and collection. Collection
is blocked while a session is running or its exit is unconfirmed, including
an abandoned active marker after a launcher restart. Identity-mismatched content
is omitted and classified as damaged. The previous structured stream can be
collected even when its raw stream is missing. Legacy sessions stay explicitly
unverified; they are not retroactively assigned a fabricated identity.

Launcher/core metadata use bounded, validated schemas. They record versions,
builds, observed startup stages and process creation. Process and guest exits
are separate; absent values are written as unknown, never substituted with zero.
The last successfully persisted core stage is an observation, not proof of a
failure's cause. No environment, command line, account, hostname or local path
is exported. All package members receive a final privacy scan. No game is
classified as a crash or incompatible from its exit code.

Reports use a fresh temporary directory and a bounded, process-free ZIP STORE
writer. No compaction subprocess exists to time out or continue after cleanup.
Output diagnostics are limited to 4 MiB each to preserve the existing 10 MiB
upload limit; source logs and structured records are not truncated. A limited
export is explicitly partial and cannot be sent automatically. STORE archives
are larger than compressed archives; the writer checks the total package limit.
Mandatory quality/session metadata are read back against their canonical values.
The archive's exact member list, sizes, CRCs, offsets and end records are checked
before a .partial file is atomically renamed to .zip without replacement.
Failure paths remove only that collection's own files. Prior packages and stale
legacy temporary directories are not included or deleted. Historical session
directories are retained; retention/cleanup policy is separate work.
These checks do not add game execution, telemetry, automatic consent or a network
destination. Saving/cancelling still makes no request, and metadata use the same
manual report decision. If incomplete-report sending is declined, the validated
local copy is retained and nothing is uploaded.

## Local synthetic validation

After building the corresponding targets, run only the selected tests:

```text
ctest --test-dir build -R "^(log_record|log_record_file|launcher_report_quality|launcher_sessions|launcher_privacy|launcher_localization)$" --output-on-failure
python source/tools/check_localization.py
```

`test_log_record_file` uses a synthetic child to exercise the production Windows
termination path without initializing a game or GPU. It verifies error states,
rejected serialization, size limits and concurrent writes. `test_launcher_quality`
exercises the actual collector and builder with local synthetic fixtures and a
network tripwire. It covers post-filter descriptions in all 15 interface locales,
useful/empty/filtered/damaged/missing/limited diagnostics and local drafts.

The session/packaging fixture additionally covers atomic-index replacement
failure, active leases, an unconfirmed prior session, missing previous raw logs,
identity mismatch, unknown versus known-zero exits, nine output/archive failures,
private metadata, stale attachments, bounded large logs and a network tripwire.
Native dialogs and real games require separate validation; these tests make no
claim that the original support case is resolved.
