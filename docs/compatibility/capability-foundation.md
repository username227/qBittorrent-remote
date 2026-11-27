# Phase 3A.2 — capability foundation and scope

The release inventory is the locked stable 4.1.0–5.3.1 set; the operator subsequently approved ascending audit order and subsystem implementation. CMake compiles the identities from the existing lock, so an arbitrary version prefix, unknown point release, packaged suffix or prerelease cannot be mistaken for an inventoried stable release. A connection is not refused solely on that basis at this checkpoint. Unknown and unsupported are separate results; the affected controls and guarded dispatch use their explanations. Daemon/API family recognition is not proof of full release compatibility.

## Boundary policy

| Capability | First stable release indicated by pinned controller evidence | Current gate |
|---|---|---|
| Tracker URL editing | 4.1.5 | Earlier unsupported; legacy origUrl until 5.2.0, modern url from 5.2.0 |
| Tracker tiers | 5.2.0 | Require daemon plus existing API 2.13.0 minimum; contradictory evidence unknown |
| Selected reannounce | 5.2.0 | Separate from tracker URL syntax; no broad fallback |
| Piece availability | 5.2.0 | Daemon version gate |
| Comment editing | 5.2.0 | Daemon version gate |
| Path free space | 5.3.0 | Require daemon plus existing API 2.16.0 minimum; contradictory evidence unknown |
| Modern advanced preferences, banned ranges | Existing 5.3.0 gate | Preserved policy; complete source/build semantics pending |

The existing API thresholds are not asserted as stable introduction numbers. The pinned 5.2.0 declared API is 2.15.1; 5.3.0 is 2.16.2. The tracker syntax and selected reannounce contracts are independent even at a shared release boundary. See contract-ledger.json for immutable evidence and partial-review status.

This is not a complete release compatibility matrix: the inventory includes all release tags but intermediate helper/serializer/build semantics have not all been reviewed. The policy test enumerates all 68 releases and checks these provisional gates against independently specified expected release families. Scoped authenticated HTTP profiles independently reject absent optional actions and unexpected parameters for 4.1.0, 5.1.4, 5.2.0 and 5.3.0; they are not complete historical emulators. Existing native comment and tracker GUI workflows now use the 5.1.4/5.2.0 boundary. Exact authentication/error responses, 4.1.0 synchronization and preferences, other subsystem routing, and full real-daemon workflows remain in subsequent checkpoints.

Unknown version policy is deliberately conservative for these optional operations while ordinary connection remains available. Neither the lab daemon nor production servers have been modified. The complete historical matrix, field-level capability/build probing, unsupported-control review, reconnect/stale-reply coverage, and verified support floor remain open until the applicable evidence and functional tests establish them.
