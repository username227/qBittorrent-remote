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

    def test_app_session_review_covers_release_lock(self):
        lock = json.loads((ROOT / "releases.lock.json").read_text(encoding="utf-8"))
        review = json.loads((ROOT / "app-session-review.json").read_text(encoding="utf-8"))
        rows = review["releases"]
        self.assertEqual([(r["version"], r["commit"]) for r in rows],
                         [(r["version"], r["commit"]) for r in lock["releases"]])
        self.assertEqual(len({r["app_controller_blob"] for r in rows}), 33)
        self.assertEqual(len({r["web_application_blob"] for r in rows}), 28)
        for r in rows:
            v = tuple(map(int, r["version"].split(".")))
            self.assertEqual(r["build_info"], "absent" if v < (4, 2, 0) else
                             "object-without-platform" if v < (5, 0, 0) else "object-with-platform")
            self.assertEqual(r["process_info"], "absent" if v < (5, 2, 0) else "object-launch_time")
            self.assertEqual(r["protected_no_session_http"], 403)
            self.assertRegex(r["app_controller_blob"], r"^[0-9a-f]{40}$")
            self.assertRegex(r["web_application_blob"], r"^[0-9a-f]{40}$")
        self.assertEqual(rows[-1]["qt_version_source"], "runtime-helper")

    def test_main_sync_review_covers_release_lock(self):
        lock = json.loads((ROOT / "releases.lock.json").read_text(encoding="utf-8"))
        review = json.loads((ROOT / "main-sync-review.json").read_text(encoding="utf-8"))
        controllers = json.loads((ROOT / "connection-controller-review.json").read_text(encoding="utf-8"))
        rows = review["releases"]
        self.assertEqual([(r["version"], r["commit"]) for r in rows],
                         [(r["version"], r["commit"]) for r in lock["releases"]])
        self.assertEqual([r["sync_controller_blob"] for r in rows],
                         [r["sync_controller_blob"] for r in controllers["releases"]])
        self.assertEqual(len({r["sync_controller_blob"] for r in rows}), 26)
        for r in rows:
            v = tuple(map(int, r["version"].split(".")))
            for field, boundary in (("free_space_on_disk", (4, 1, 4)),
                                    ("tags", (4, 2, 0)),
                                    ("tracker_memberships", (4, 3, 0)),
                                    ("external_addresses", (5, 1, 0)),
                                    ("session_state", (5, 3, 0)),
                                    ("request_latency", (5, 3, 0)),
                                    ("queued_tracker_announces", (5, 3, 0))):
                self.assertEqual(r[field], v >= boundary, (r["version"], field))
            self.assertEqual(r["use_subcategories"], (4, 6, 0) <= v < (5, 2, 0))
            self.assertEqual(r["delta_engine"], "event-buffer" if v >= (4, 6, 0) else "snapshot-diff")
            self.assertEqual(r["response_id_cycle"], [1, 1000000])
            self.assertEqual(r["tracker_delta"], "replace-membership-per-url" if r["tracker_memberships"] else "absent")

    def test_peer_sync_review_covers_release_lock(self):
        lock = json.loads((ROOT / "releases.lock.json").read_text(encoding="utf-8"))
        review = json.loads((ROOT / "peer-sync-review.json").read_text(encoding="utf-8"))
        controllers = json.loads((ROOT / "connection-controller-review.json").read_text(encoding="utf-8"))
        rows = review["releases"]
        self.assertEqual([(r["version"], r["commit"]) for r in rows],
                         [(r["version"], r["commit"]) for r in lock["releases"]])
        self.assertEqual([r["sync_controller_blob"] for r in rows],
                         [r["sync_controller_blob"] for r in controllers["releases"]])
        self.assertEqual(len({r["sync_controller_blob"] for r in rows}), 26)
        for r in rows:
            v = tuple(map(int, r["version"].split(".")))
            self.assertEqual(r["peer_id_client"], v >= (4, 5, 0))
            self.assertEqual(r["host_name"], v >= (5, 2, 0))
            self.assertEqual(r["i2p_dest"], v >= (5, 2, 0))
            self.assertEqual(r["contribution"], v >= (5, 3, 0))
            self.assertEqual(r["country_disabled"], "explicit-empty-values" if v >= (5, 2, 0) else "omitted")
            self.assertTrue(r["show_flags"])

    def test_torrent_serializer_inventory_covers_release_lock(self):
        lock = json.loads((ROOT / "releases.lock.json").read_text(encoding="utf-8"))
        review = json.loads((ROOT / "torrent-serializer-review.json").read_text(encoding="utf-8"))
        rows, revisions = review["releases"], review["revisions"]
        self.assertEqual(review["status"], "lexical-source-inventory")
        self.assertEqual([(r["version"], r["commit"]) for r in rows],
                         [(r["version"], r["commit"]) for r in lock["releases"]])
        self.assertEqual(len(revisions), 19)
        self.assertEqual(len({(r["cpp_blob"], r["header_blob"]) for r in revisions}), len(revisions))
        previous, union, visited, first_seen = set(), set(), set(), {}
        for row in rows:
            i = row["revision"]
            self.assertTrue(0 <= i < len(revisions))
            rev = revisions[i]
            if i not in visited:
                self.assertEqual(rev["first_release"], row["version"])
            visited.add(i)
            for name in ("cpp_blob", "header_blob"):
                self.assertRegex(rev[name], r"^[0-9a-f]{40}$")
            fields = rev["fields"]
            keys = {f["key"] for f in fields}
            self.assertEqual(len(keys), len(fields))
            for field in fields:
                self.assertTrue(field["assignments"])
                self.assertTrue(all("KEY_TORRENT_" in a for a in field["assignments"]))
            self.assertEqual(row["added"], sorted(keys - previous))
            self.assertEqual(row["removed"], sorted(previous - keys))
            for key in keys:
                first_seen.setdefault(key, row["version"])
            previous = keys
            union.update(keys)
        self.assertEqual(visited, set(range(len(revisions))))
        self.assertEqual(review["counts"], {"releases": len(rows),
                         "adjacent_pairs": len(rows) - 1, "revisions": len(revisions),
                         "distinct_fields": len(union)})
        self.assertEqual((len(revisions[0]["fields"]), len(revisions[-1]["fields"])), (42, 67))
        for key, boundary in (("availability", "4.2.0"), ("trackers_count", "4.3.0"),
                              ("content_path", "4.3.1"), ("seeding_time", "4.3.4"),
                              ("infohash_v1", "4.4.0"), ("infohash_v2", "4.4.0"),
                              ("download_path", "4.4.0"), ("inactive_seeding_time_limit", "4.6.0"),
                              ("private", "5.0.0"), ("reannounce", "5.0.0"),
                              ("creation_date", "5.2.0"), ("share_limit_action", "5.2.0"),
                              ("share_limits_mode", "5.3.0")):
            self.assertEqual(first_seen[key], boundary, key)

    def test_torrent_metric_backend_review_covers_release_lock(self):
        lock = json.loads((ROOT / "releases.lock.json").read_text(encoding="utf-8"))
        report = json.loads((ROOT / "torrent-metric-backend-review.json").read_text(encoding="utf-8"))
        rows, methods = report["releases"], report["methods"]
        self.assertEqual([(r["version"], r["commit"]) for r in rows],
                         [(r["version"], r["commit"]) for r in lock["releases"]])
        self.assertEqual(len({r["blob"] for r in rows}), 44)
        self.assertEqual(report["counts"]["methods"], len(methods))
        self.assertEqual(report["counts"]["method_variants"], sum(map(len, methods.values())))
        visited = {name: set() for name in methods}
        for row in rows:
            self.assertRegex(row["blob"], r"^[0-9a-f]{40}$")
            self.assertEqual(set(row["methods"]), set(methods))
            for name, i in row["methods"].items():
                if i is None:
                    continue
                self.assertTrue(0 <= i < len(methods[name]))
                if i not in visited[name]:
                    self.assertEqual(methods[name][i]["first_release"], row["version"])
                visited[name].add(i)
        for name, variants in methods.items():
            self.assertEqual(visited[name], set(range(len(variants))))
        for name in ("wantedSize", "totalSize", "completedSize", "totalDownload",
                     "totalUpload", "totalPayloadDownload", "totalPayloadUpload"):
            self.assertEqual(len(methods[name]), 1, name)
        self.assertEqual(methods["progress"][1]["first_release"], "4.1.1")
        self.assertEqual(methods["totalSeedsCount"][1]["first_release"], "4.5.0")
        self.assertEqual(methods["activeTime"][1]["first_release"], "4.2.0")

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
