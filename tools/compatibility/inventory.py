#!/usr/bin/env python3
"""Generate source evidence for Phase 3A from an upstream Git object database.

This is an inventory, NOT a verified endpoint/schema or capability ledger.
No checkout, network access, or source execution is performed.
"""
import argparse
import functools
import json
import re
import subprocess
from pathlib import Path

MIN = (4, 1, 0)
MAX = (5, 3, 1)
TAG = re.compile(r"release-(\d+\.\d+\.\d+(?:\.\d+)?)\Z")
ACTION = re.compile(r"\b([A-Za-z_]\w*Controller)::([A-Za-z_]\w*?)Action\s*\(")
PINS = {
    "release-4.1.0": "e9ed621178af841114a045887c5ce1e6b50cb366",
    "release-4.1.1": "ea5a29018fba738223d0f1143ac97722e3ee9b5f",
    "release-5.3.1": "f3cfbccd42dc07e7aa174b525f6d6dfd2d0595a0",
}


def git(repo, *args, allow_no_match=False):
    p = subprocess.run(["git", "-C", str(repo), *args],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       check=False)
    if p.returncode and not (allow_no_match and p.returncode == 1):
        raise RuntimeError(p.stderr.decode("utf-8", "replace").strip()
                           or f"git {args[0]} failed ({p.returncode})")
    return p.stdout


def releases(repo):
    tags = git(repo, "for-each-ref", "--format=%(refname:strip=2)",
               "refs/tags").decode().splitlines()
    result = []
    for tag in tags:
        match = TAG.fullmatch(tag)
        if not match:
            continue
        parts = tuple(map(int, match.group(1).split(".")))
        if not MIN <= parts <= MAX:
            continue
        sha = git(repo, "rev-parse", "--verify",
                  f"refs/tags/{tag}^{{commit}}").decode().strip()
        result.append({"tag": tag, "version": match.group(1), "commit": sha})
    result.sort(key=lambda row: tuple(map(int, row["version"].split("."))))
    if not result or result[0]["version"] != "4.1.0" or result[-1]["version"] != "5.3.1":
        raise ValueError("Missing range endpoints; fetch the complete upstream tag set")
    by_tag = {row["tag"]: row["commit"] for row in result}
    for tag, sha in PINS.items():
        if by_tag.get(tag) != sha:
            raise ValueError(f"Missing or changed independently pinned tag: {tag}")
    if len({tuple(map(int, r["version"].split("."))) for r in result}) != len(result):
        raise ValueError("Duplicate numeric version; resolve tag aliases explicitly")
    if len({r["commit"] for r in result}) != len(result):
        raise ValueError("Multiple stable version tags resolve to one commit; review aliases")
    return result


def tree(repo, sha):
    result = {}
    for entry in git(repo, "ls-tree", "-r", "-z", sha).split(b"\0"):
        if not entry:
            continue
        metadata, path = entry.split(b"\t", 1)
        mode, kind, oid = metadata.decode().split()
        result[path.decode("utf-8")] = {"mode": mode, "type": kind, "oid": oid}
    return result


def changes(before, after):
    return [{"path": path, "before": before.get(path), "after": after.get(path)}
            for path in sorted(before.keys() | after.keys())
            if before.get(path) != after.get(path)]


def action_candidates(source):
    # Lexical matches, including potential comments or conditional code.
    # Dispatch, visibility, aliases, methods and semantics require review.
    return [{"controller": m.group(1), "action": m.group(2),
             "line": source.count("\n", 0, m.start()) + 1}
            for m in ACTION.finditer(source)]


def build(repo):
    rows = releases(repo)
    previous_tree = None
    previous = None
    deltas = []

    @functools.lru_cache(maxsize=512)
    def source(oid):
        return git(repo, "cat-file", "blob", oid).decode("utf-8", "replace")

    for row in rows:
        sha = row["commit"]
        files = tree(repo, sha)
        candidates = []
        web_sources = {}
        for path, item in sorted(files.items()):
            if not path.startswith("src/webui/") or "/www/" in path:
                continue
            web_sources[path] = item
            if not path.startswith("src/webui/api/") or not path.endswith(".cpp"):
                continue
            if item["type"] != "blob":
                continue
            for action in action_candidates(source(item["oid"])):
                candidates.append({"path": path, "blob": item["oid"], **action})
        version_lines = git(repo, "grep", "-n", "-I", "-E",
                            r"(^|[^[:alnum:]_])API_VERSION([^[:alnum:]_]|$)",
                            sha, "--", "src", allow_no_match=True)
        row["api_version_source_candidates"] = version_lines.decode("utf-8", "replace").splitlines()
        row["web_sources"] = web_sources
        row["action_candidates"] = candidates
        row["contract_status"] = "unreviewed"
        if previous_tree is not None:
            old_actions = {(a["controller"], a["action"]) for a in previous["action_candidates"]}
            new_actions = {(a["controller"], a["action"]) for a in candidates}
            deltas.append({
                "from": previous["version"], "to": row["version"],
                "from_commit": previous["commit"], "to_commit": sha,
                "changed_tree_entries": changes(previous_tree, files),
                "added_action_candidates": sorted(new_actions - old_actions),
                "removed_action_candidates": sorted(old_actions - new_actions),
                "contract_status": "unreviewed",
            })
        previous_tree, previous = files, row
    return {"schema": 1, "upstream": "https://github.com/qbittorrent/qBittorrent",
            "direction": "ascending", "releases": rows, "adjacent_deltas": deltas}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("upstream", type=Path, help="upstream Git checkout or bare repository")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--verify-lock", type=Path,
                        help="existing inventory; require identical ordered version/commit pins")
    args = parser.parse_args()
    try:
        report = build(args.upstream)
        if args.verify_lock:
            old = json.loads(args.verify_lock.read_text(encoding="utf-8"))
            def pins(data):
                return [(r["tag"], r["commit"]) for r in data["releases"]]
            if pins(old) != pins(report):
                raise ValueError("Release inventory differs from lock; review tags before proceeding")
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n",
                               encoding="utf-8")
    except (OSError, RuntimeError, ValueError, KeyError) as exc:
        parser.exit(1, f"audit: {exc}\n")
    print(f"{len(report['releases'])} releases; {len(report['adjacent_deltas'])} adjacent comparisons")


if __name__ == "__main__":
    main()
