"""Offline unit checks for the inventory primitives; no upstream or network."""
import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "inventory", Path(__file__).with_name("inventory.py"))
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


class InventoryTests(unittest.TestCase):
    def test_stable_tags(self):
        for tag in ("release-4.1.0", "release-4.1.9.1", "release-5.3.1"):
            self.assertIsNotNone(inventory.TAG.fullmatch(tag))
        for tag in ("release-5.3.1rc1", "release-4.1.0beta1", "v4.1.0", "release-4.1"):
            self.assertIsNone(inventory.TAG.fullmatch(tag))

    def test_actions(self):
        source = "void AppController::versionAction()\n{}\nvoid AuthController::loginAction() {}"
        self.assertEqual(inventory.action_candidates(source), [
            {"controller": "AppController", "action": "version", "line": 1},
            {"controller": "AuthController", "action": "login", "line": 3},
        ])

    def test_full_tree_delta(self):
        old = {"same": {"oid": "a"}, "delete": {"oid": "b"},
               "src/base/preferences.cpp": {"oid": "c", "mode": "100644"}}
        new = {"same": {"oid": "a"}, "add": {"oid": "d"},
               "src/base/preferences.cpp": {"oid": "c", "mode": "100755"}}
        self.assertEqual([x["path"] for x in inventory.changes(old, new)],
                         ["add", "delete", "src/base/preferences.cpp"])
        self.assertEqual(inventory.changes(old, old), [])
        self.assertIsNone(inventory.changes(old, new)[0]["before"])


if __name__ == "__main__":
    unittest.main()
