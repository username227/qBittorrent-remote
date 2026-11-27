"""Offline checks of the locked stable-release inventory and historical evidence."""
import copy
import hashlib
import json
import unittest
from pathlib import Path

from summarize_inventory import summarize, pins
from capture_baseline import decode_response_body, shape

ROOT = Path(__file__).resolve().parents[2] / "docs" / "compatibility"


class EvidenceTests(unittest.TestCase):
    def test_locked_action_history(self):
        lock = json.loads((ROOT / "releases.lock.json").read_text(encoding="utf-8"))
        history = json.loads((ROOT / "action-history.json").read_text(encoding="utf-8"))
        releases = lock["releases"]
        versions = [r["version"] for r in releases]
        self.assertEqual(versions, sorted(set(versions), key=lambda v: tuple(map(int, v.split(".")))))
        self.assertEqual((versions[0], versions[-1]), ("4.1.0", "5.3.1"))
        for row in releases:
            self.assertRegex(row["version"], r"^\d+\.\d+\.\d+(?:\.\d+)?$")
            self.assertEqual(row["tag"], "release-" + row["version"])
            self.assertRegex(row["commit"], r"^[0-9a-f]{40}$")
        self.assertEqual(len({r["commit"] for r in releases}), len(releases))
        pin_bytes = "".join(f"{tag} {sha}\n" for tag, sha in pins(releases)).encode("ascii")
        self.assertEqual(history["release_pins_sha256"], hashlib.sha256(pin_bytes).hexdigest())
        self.assertEqual(history["status"], "lexical-unreviewed")
        self.assertEqual([r["version"] for r in history["releases"]], versions)
        self.assertEqual(history["counts"]["releases"], len(releases))
        self.assertEqual(history["counts"]["adjacent_pairs"], len(releases) - 1)
        actions, all_actions = set(), set()
        changes = 0
        for i, row in enumerate(history["releases"]):
            added, removed = set(row["added"]), set(row["removed"])
            self.assertEqual(len(added), len(row["added"]))
            self.assertEqual(len(removed), len(row["removed"]))
            self.assertFalse(added & actions)
            self.assertTrue(removed <= actions)
            self.assertFalse(added & removed)
            actions = (actions - removed) | added
            all_actions.update(actions)
            self.assertEqual(len(actions), row["action_count"])
            if i:
                self.assertGreaterEqual(row["changed_tree_entries"], 0)
                changes += row["changed_tree_entries"]
            else:
                self.assertIsNone(row["changed_tree_entries"])
        self.assertEqual(len(all_actions), history["counts"]["distinct_actions"])
        self.assertEqual(changes, history["counts"]["changed_tree_entries"])
        self.assertEqual((history["releases"][0]["action_count"], len(actions)), (61, 132))

    def test_connection_controller_review_covers_release_lock(self):
        lock = json.loads((ROOT / "releases.lock.json").read_text(encoding="utf-8"))
        review = json.loads((ROOT / "connection-controller-review.json").read_text(encoding="utf-8"))
        expected = [(r["version"], r["commit"]) for r in lock["releases"]]
        rows = review["releases"]
        self.assertEqual([(r["version"], r["commit"]) for r in rows], expected)
        self.assertEqual(len({r["sync_controller_blob"] for r in rows}), 26)
        self.assertEqual(len({r["auth_controller_blob"] for r in rows}), 9)
        self.assertEqual([r["version"] for r in rows if r["categories_wire_shape"] == "name-array"],
                         ["4.1.0", "4.1.1", "4.1.2"])
        self.assertEqual([r["version"] for r in rows if r["login_controller_contract"] == "APIStatus::Ok/Unauthorized"],
                         ["5.2.0", "5.2.1", "5.2.2", "5.2.3", "5.2.4", "5.3.0", "5.3.1"])
        for r in rows:
            self.assertRegex(r["sync_controller_blob"], r"^[0-9a-f]{40}$")
            self.assertRegex(r["auth_controller_blob"], r"^[0-9a-f]{40}$")
            self.assertIn(r["categories_wire_shape"], ("name-array", "definition-map"))
            self.assertIn(r["login_controller_contract"], ("Ok./Fails. text", "APIStatus::Ok/Unauthorized"))
            self.assertEqual(r["full_connection_contract"], "pending")

    def test_empty_4_1_0_live_evidence(self):
        evidence = json.loads((ROOT / "evidence/qbt-4.1.0-baseline.json").read_text(encoding="utf-8"))
        self.assertEqual(evidence["expected_daemon"], "4.1.0")
        self.assertEqual(evidence["login"]["status"], 200)
        responses = evidence["responses"]
        self.assertEqual(len(responses), 10)
        self.assertEqual(responses["app/version"]["body"], "v4.1.0")
        # Original capture decoded the text/plain wire response as a JSON number.
        self.assertEqual(responses["app/webapiVersion"]["body"], 2.0)
        self.assertEqual(responses["app/webapiVersion"]["content_type"], "text/plain")
        for endpoint in ("app/buildInfo", "torrents/categories", "torrents/tags"):
            self.assertEqual(responses[endpoint], {"status": 404})
        self.assertEqual(responses["torrents/info"]["body"], [])
        sync = responses["sync/maindata?rid=0"]["body"]
        self.assertTrue(sync["full_update"])
        self.assertEqual(sync["categories"], [])
        self.assertEqual(sync["torrents"], {})
        self.assertNotIn("tags", sync)
        self.assertEqual(sync["server_state"]["global_ratio"], "str")
        self.assertEqual(len(responses["app/preferences"]["shape"]), 89)
        self.assertEqual(responses["app/preferences"]["shape"]["RSSProcessingEnabled"], "bool")

    def test_capture_preserves_wire_text_and_redacts_nested_keys(self):
        self.assertEqual(decode_response_body("text/plain; charset=utf-8", b"2.0"), "2.0")
        self.assertEqual(decode_response_body("text/plain", b"0"), "0")
        self.assertEqual(decode_response_body("application/json", b"[]"), [])
        self.assertEqual(shape({"scan_dirs": {"/private/path": 1}, "rss": True}),
                         {"scan_dirs": {"redacted_type": "dict", "count": 1}, "rss": "bool"})

    def test_summarizer_validates_source_and_adjacent_deltas(self):
        a = {"tag": "release-4.1.0", "version": "4.1.0", "commit": "a" * 40}
        b = {"tag": "release-4.1.1", "version": "4.1.1", "commit": "b" * 40}
        header = {"schema": 1, "upstream": "https://github.com/qbittorrent/qBittorrent", "direction": "ascending"}
        lock = {**header, "releases": [a, b]}
        source = {"src/webui/api/appcontroller.cpp": {"oid": "c" * 40}}
        candidate = {"path": next(iter(source)), "blob": "c" * 40,
                     "controller": "AppController", "action": "version", "line": 42}
        report = {**header, "releases": [
            {**a, "web_sources": source, "action_candidates": [candidate]},
            {**b, "web_sources": source, "action_candidates": []}],
            "adjacent_deltas": [{"from": a["version"], "to": b["version"],
                                 "from_commit": a["commit"], "to_commit": b["commit"],
                                 "added_action_candidates": [], "removed_action_candidates": [["AppController", "version"]],
                                 "changed_tree_entries": []}]}
        def run(data):
            return summarize(json.dumps(data).encode(), lock)
        result = run(report)
        self.assertEqual(result["releases"][1]["removed"], ["AppController::version"])
        self.assertEqual(result["counts"]["distinct_actions"], 1)
        for mutate in (
                lambda r: r["releases"][0].update(commit="d" * 40),
                lambda r: r["releases"][0]["action_candidates"][0].update(blob="d" * 40),
                lambda r: r["adjacent_deltas"][0].update(removed_action_candidates=[]),
                lambda r: r["adjacent_deltas"].clear()):
            invalid = copy.deepcopy(report)
            mutate(invalid)
            with self.assertRaises(ValueError):
                run(invalid)


if __name__ == "__main__":
    unittest.main()
