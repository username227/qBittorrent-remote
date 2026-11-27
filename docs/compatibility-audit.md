# Phase 3A — upstream source inventory (initial tooling)

The compatibility floor is provisionally qBittorrent 4.1.0 / Web API 2.0.
The reference remains 5.3.1. Following successful launch of 4.1.0 in an
isolated QNAP container on 2026-11-27, the audit order is ascending, comparing
each stable release with its immediate predecessor.

## Generate the inventory

Use a complete upstream Git repository (all stable release tags, not a
shallow checkout):

```sh
git clone --bare https://github.com/qbittorrent/qBittorrent.git qbittorrent-upstream.git
python3 tools/compatibility/inventory.py qbittorrent-upstream.git --output upstream-inventory.json
python3 tools/compatibility/summarize_inventory.py upstream-inventory.json --lock docs/compatibility/releases.lock.json --output /tmp/action-history.json
cmake --preset dev
ctest --preset dev -L compatibility
```

The tool reads Git objects without checking out or executing upstream code.
It recognizes exact numeric release tags with three or four components,
excludes prerelease suffixes and other tag forms, numerically sorts the
inclusive range, checks independently verified anchor commits, and fails
on duplicate numeric versions or shared commit aliases for manual review.
Completeness still depends on the full upstream tag set. Once reviewed,
use `--verify-lock docs/compatibility/releases.lock.json` on a subsequent generation
to reject tag/commit drift. Do not overwrite the lock when validating it.

Every release includes immutable commit/blob IDs, candidate C++ actions,
WebUI source hashes, and API_VERSION references. Every adjacent pair
includes all changed tree entries (including dependencies, build files,
helpers, and deletions) and lexical action additions/removals. A rename
appears as a removal and an addition. No unchanged controller or empty
delta establishes semantic equivalence. All contract entries explicitly
remain unreviewed until dispatch, authentication, HTTP methods, request
and response schemas, preferences, conditional builds and backend
behavior have been investigated. Auth/route special cases are not captured
by the action regex. This is not yet the final semantic ledger or CTest
release matrix.

## Initial evidence

- 4.1.0: e9ed621178af841114a045887c5ce1e6b50cb366
- 4.1.1: ea5a29018fba738223d0f1143ac97722e3ee9b5f
- 5.3.1: f3cfbccd42dc07e7aa174b525f6d6dfd2d0595a0

At 4.1.0, src/webui/api/appcontroller.cpp (blob
694cd59bd7c17d419fa1180c5438856863a7ff0b) defines webapiVersion,
version, shutdown, preferences, setPreferences and defaultSavePath
actions. The 5.3.1 counterpart (blob
a9e8c1eae009fdcccc7e5de6216c70e20c58e034) also defines buildInfo,
among other additions. The inventory places the first lexical buildInfo candidate at 4.2.0; dispatch and build conditions still require review.

The 4.1.0 -> 4.1.1 upstream comparison shows modifications to
appcontroller.cpp, serialize/serialize_torrent.cpp, synccontroller.cpp,
torrentscontroller.cpp, and webapplication.cpp/.h. These are investigation
leads, not findings about wire behavior.

Live baseline: user verified 4.1.0 executable, WebUI login and reported
Web API 2.0. Image:
wernight/qbittorrent@sha256:f4504b29dce8f4cddcc3e0fe2e6a3410269a41e6843ffc8cb99e0c923f3dee4a.
Native client workflow coverage is not yet established. No production
data or credentials belong in this audit. The final ledger, strict
independent fixtures, and CTest matrix will follow after inventory review.

## Validated inventory and first findings

On 2026-11-27 the operator ran all three inventory primitive tests and
generated the complete inventory (68 releases, 67 adjacent comparisons).
The uploaded inventory was parsed in full: 27 distinct declared API
versions, 61 lexical action candidates at the floor and 132 at the
reference, 16,373 changed tree entries in total. These are not endpoint
counts. The ordered release lock is in compatibility/releases.lock.json
and the first partial source findings in compatibility/contract-ledger.json.
The lock was transcribed from the generated report and must be checked
with --verify-lock on regeneration. Declared numeric API versions are not
necessarily formatted wire strings (4.1.0 declares 2.0.0, reports 2.0).

Twenty adjacent pairs retain the same declared API version despite
non-frontend WebUI source changes. In particular 4.1.8 adds an RSS
refreshItem candidate (2.2.0 unchanged) and 4.3.3 adds a renameFolder
candidate (2.7.0 unchanged). Neither can be safely gated by API version
alone without reviewing routing and conditional availability. The
inventory also identifies declared API 2.11.4 for 5.1.4 and 2.16.2 for
5.3.0/5.3.1; do not infer installed package wire values from these alone.

The first partial findings establish the historical category array,
4.1.0 RSS preference key names and unconditional writes, the 4.1.1
preference fix, the addition of the hashes filter in torrents/info and
the moving state serialization. See source evidence in the ledger.
Every remaining contract and adjacent pair remains explicitly pending;
there is no completed semantic release audit yet.

## Inventory checkpoint and observed historical evidence

The checked-in `compatibility/action-history.json` is a compact, reproducible
lexical summary of all 68 locked stable releases and 67 adjacent comparisons.
It records 6,481 candidate records, 135 distinct action names, 382 unique WebUI
source blobs and 16,373 changed tree entries. The raw report remains a generated
artifact; its SHA-256 and the canonical release-pin digest are recorded in the
summary. The summarizer rejects inconsistent release pins, action source
references and adjacent deltas. Empty action deltas do not imply semantic
compatibility. No prereleases are in scope.

`compatibility/evidence/qbt-4.1.0-baseline.json` preserves the operator's original
sanitized live capture byte for byte: successful login, seven successful read
probes, three HTTP 404 results, 89 preference fields, and an empty full sync
with array-valued categories. This is shape evidence, not a populated daemon
fixture or a verified mutation contract. The original capture decoded numeric
plain-text responses as JSON numbers; future captures preserve plain-text
version and mode strings and redact nested user-defined keys. No new daemon
capture was performed for this checkpoint.

Python 3 is a test-build dependency only. Both offline tooling test suites run
through CTest under the `compatibility` label; CI installs the interpreter on
Linux and Windows. These checks validate evidence integrity, not GUI or remote
protocol compatibility. Native runtime implementation and functional historical
server workflows begin in subsequent checkpoints, by subsystem, reviewing each
stable release in ascending order and sharing equivalent implementations. The
4.1.0 support floor remains provisional until actual verification.
