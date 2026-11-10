import copy
import unittest

from validate_workflow import sample_packet, validate


class WorkflowValidationTests(unittest.TestCase):
    def setUp(self):
        self.packet = sample_packet()

    def test_independent_tasks_advance_separately(self):
        tasks = self.packet["tasks"]
        self.assertEqual(tasks[0]["stage"], "review")
        self.assertEqual(tasks[1]["stage"], "implement")
        self.assertEqual(tasks[0]["dependencies"], [])
        self.assertEqual(tasks[1]["dependencies"], [])
        self.assertEqual(validate(self.packet), [])

    def test_empty_task_list_is_rejected(self):
        self.packet["tasks"] = []
        self.assertTrue(any("at least one task" in e for e in validate(self.packet)))

    def test_cycle_is_rejected(self):
        tasks = self.packet["tasks"]
        tasks[0]["dependencies"] = ["docs"]
        tasks[1]["dependencies"] = ["core"]
        self.assertTrue(any("cycle" in e for e in validate(self.packet)))

    def test_stale_gate_candidate_is_rejected(self):
        task = self.packet["tasks"][0]
        task.update(stage="gate", gates=[{
            "status": "PASS", "candidate": "old-tree", "evidence": "tests.xml", "sha256": "0" * 64,
            "author": "a", "reviewer": "r", "verifier": "v", "gatekeeper": "g",
        }])
        self.assertTrue(any("candidate does not match" in e for e in validate(self.packet)))

    def test_self_review_is_rejected(self):
        task = self.packet["tasks"][0]
        task.update(stage="gate", gates=[{
            "status": "PASS", "candidate": task["candidate"],
            "author": "same", "reviewer": "same", "verifier": "v", "gatekeeper": "g",
        }])
        self.assertTrue(any("independent" in e for e in validate(self.packet)))

    def test_unavailable_mandatory_gate_is_not_pass(self):
        task = self.packet["tasks"][0]
        task.update(stage="gate", gates=[{
            "candidate": task["candidate"], "evidence": "test.xml", "sha256": "0" * 64,
            "author": "a", "reviewer": "r", "verifier": "v", "gatekeeper": "g",
        }])
        errors = validate(self.packet)
        self.assertTrue(any("never passes" in e for e in errors))

    def test_duplicate_and_missing_dependency_rejected(self):
        self.packet["tasks"][0]["dependencies"] = ["absent"]
        self.packet["tasks"].append(copy.deepcopy(self.packet["tasks"][1]))
        errors = validate(self.packet)
        self.assertTrue(any("missing dependency" in e for e in errors))
        self.assertTrue(any("duplicate task id" in e for e in errors))

    def test_advanced_stage_requires_candidate(self):
        self.packet["tasks"][0]["candidate"] = "  "
        self.assertTrue(any("requires a nonempty candidate" in e for e in validate(self.packet)))

    def test_gate_waits_for_dependencies_to_settle(self):
        tasks = self.packet["tasks"]
        tasks[0]["stage"] = "settled"
        tasks[0]["gates"] = [{
            "status": "PASS", "candidate": tasks[0]["candidate"], "evidence": "test-core.xml", "sha256": "1" * 64,
            "author": "a", "reviewer": "r", "verifier": "v", "gatekeeper": "g",
        }]
        tasks[1]["stage"] = "gate"
        tasks[1]["dependencies"] = ["core"]
        tasks[1]["gates"] = [{
            "status": "PASS", "candidate": tasks[1]["candidate"], "evidence": "test-docs.xml", "sha256": "2" * 64,
            "author": "a2", "reviewer": "r2", "verifier": "v2", "gatekeeper": "g2",
        }]
        self.assertEqual(validate(self.packet), [])
        tasks[0]["stage"] = "review"
        self.assertTrue(any("dependency core is not settled" in e for e in validate(self.packet)))


if __name__ == "__main__":
    unittest.main()
