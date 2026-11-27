#!/usr/bin/env python3
"""Verify a generated upstream inventory against its lock and compact action history.

This is lexical source evidence, not a verified Web API capability matrix.
No Git checkout or network access is needed.
"""
import argparse
import hashlib
import json
from pathlib import Path


def pins(rows):
    return [(row["tag"], row["commit"]) for row in rows]


def summarize(raw, lock):
    report = json.loads(raw)
    for key in ("schema", "upstream", "direction"):
        if report[key] != lock[key]:
            raise ValueError(f"Inventory/lock mismatch: {key}")
    if report["schema"] != 1 or report["direction"] != "ascending":
        raise ValueError("Unsupported inventory format")
    releases = report["releases"]
    deltas = report["adjacent_deltas"]
    if not releases or pins(releases) != pins(lock["releases"]):
        raise ValueError("Release pins differ from the lock")
    if len(deltas) != len(releases) - 1:
        raise ValueError("Incomplete adjacent release inventory")
    history = []
    previous = set()
    all_actions = set()
    all_blobs = set()
    candidate_records = 0
    tree_changes = 0
    for i, row in enumerate(releases):
        if row["version"] != lock["releases"][i]["version"]:
            raise ValueError("Release version differs from the lock")
        sources = row["web_sources"]
        all_blobs.update(s["oid"] for s in sources.values())
        candidates = row["action_candidates"]
        candidate_records += len(candidates)
        actions = set()
        for action in candidates:
            if sources[action["path"]]["oid"] != action["blob"] or action["line"] < 1:
                raise ValueError(f"Invalid action evidence in {row['version']}")
            actions.add((action["controller"], action["action"]))
        added, removed = actions - previous, previous - actions
        changes = None
        if i:
            delta = deltas[i - 1]
            prior = releases[i - 1]
            if (delta["from"], delta["to"], delta["from_commit"], delta["to_commit"]) != (
                    prior["version"], row["version"], prior["commit"], row["commit"]):
                raise ValueError(f"Incorrect adjacent pair at {row['version']}")
            if (delta["added_action_candidates"] != [list(a) for a in sorted(added)] or
                    delta["removed_action_candidates"] != [list(a) for a in sorted(removed)]):
                raise ValueError(f"Incorrect action delta at {row['version']}")
            changes = len(delta["changed_tree_entries"])
            tree_changes += changes
        history.append({"version": row["version"], "action_count": len(actions),
                        "changed_tree_entries": changes,
                        "added": ["::".join(a) for a in sorted(added)],
                        "removed": ["::".join(a) for a in sorted(removed)]})
        all_actions.update(actions)
        previous = actions
    pin_bytes = "".join(f"{tag} {sha}\n" for tag, sha in pins(releases)).encode("ascii")
    return {"schema": 1, "status": "lexical-unreviewed",
            "inventory_sha256": hashlib.sha256(raw).hexdigest(),
            "release_pins_sha256": hashlib.sha256(pin_bytes).hexdigest(),
            "counts": {"releases": len(releases), "adjacent_pairs": len(deltas),
                       "candidate_records": candidate_records, "distinct_actions": len(all_actions),
                       "unique_web_source_blobs": len(all_blobs), "changed_tree_entries": tree_changes},
            "releases": history}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inventory", type=Path)
    parser.add_argument("--lock", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = summarize(args.inventory.read_bytes(), json.loads(args.lock.read_text(encoding="utf-8")))
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(1, f"audit: {exc}\n")


if __name__ == "__main__":
    main()
