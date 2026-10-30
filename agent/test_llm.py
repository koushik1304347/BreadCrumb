"""Offline contract tests: no credential, quota, or network required."""
import copy
import io
import json
import os
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch
from urllib.error import HTTPError

from llm_agent import run_task
from llm_planner import Gemini


def reply(*parts):
    return {"role": "model", "parts": list(parts)}


def call(name="read_file", **args):
    return {"functionCall": {"name": name, "args": args}, "thoughtSignature": "opaque"}


class FakeModel:
    model = "offline-test"

    def __init__(self, *replies):
        self.replies = iter(replies)
        self.histories = []

    def ask(self, history, effects):
        self.histories.append(copy.deepcopy(history))
        return next(self.replies)


class FakeBridge:
    def __init__(self):
        self.actions = []
        self.events = []

    def record(self, **event):
        self.events.append(event)

    def execute(self, action, destructive=False):
        self.actions.append(action)
        return "100 requests. Five failed."


class LoopTests(unittest.TestCase):
    def setUp(self):
        self.bridge = FakeBridge()
        self.output = redirect_stdout(io.StringIO())
        self.output.__enter__()
        self.addCleanup(self.output.__exit__, None, None, None)

    def test_observation_and_signature_return_to_model(self):
        first = reply(call(path="/demo/report.txt"))
        model = FakeModel(first, reply({"text": "Five of 100 requests failed."}))
        answer = run_task(model, self.bridge, "Summarize report")
        self.assertIn("Five", answer)
        self.assertEqual(model.histories[1][1], first)
        response = model.histories[1][2]["parts"][0]["functionResponse"]
        self.assertEqual(response["response"]["output"], "100 requests. Five failed.")

    def test_batch_rejected_before_any_execution(self):
        model = FakeModel(reply(call(path="/demo/report.txt"),
                                call("delete_file", path="/demo/important.txt")))
        with self.assertRaises(ValueError):
            run_task(model, self.bridge, "Read report")
        self.assertEqual(self.bridge.actions, [])

    def test_path_escape_rejected(self):
        with self.assertRaises(ValueError):
            run_task(FakeModel(reply(call(path="/demo/../README"))), self.bridge, "Read")
        self.assertEqual(self.bridge.actions, [])

    def test_effects_require_explicit_mode(self):
        model = FakeModel(reply(call("delete_file", path="/demo/important.txt")),
                          reply({"text": "Deletion requested."}))
        run_task(model, self.bridge, "Delete important.txt", effects=True)
        self.assertEqual(self.bridge.actions[0]["tool"], "delete_file")

    def test_step_limit(self):
        with self.assertRaisesRegex(RuntimeError, "step limit"):
            run_task(FakeModel(reply(call(path="/demo/report.txt"))),
                     self.bridge, "Read", max_steps=1)

    def test_unknown_tool_rejected_in_effect_mode(self):
        with self.assertRaises(ValueError):
            run_task(FakeModel(reply(call("host_shell", command="id"))),
                     self.bridge, "Read", effects=True)
        self.assertEqual(self.bridge.actions, [])


class AdapterTests(unittest.TestCase):
    @patch.dict(os.environ, {"GEMINI_API_KEY": ""})
    def test_missing_key(self):
        with self.assertRaises(ValueError):
            Gemini()

    @patch.dict(os.environ, {"GEMINI_API_KEY": "test-key-not-real"})
    @patch("urllib.request.urlopen")
    def test_rest_contract(self, opener):
        content = reply({"text": "Hello"})
        opener.return_value.__enter__.return_value.read.return_value = json.dumps(
            {"candidates": [{"finishReason": "STOP", "content": content}]}).encode()
        self.assertEqual(Gemini().ask([], False), content)
        request = opener.call_args.args[0]
        self.assertEqual(request.get_header("X-goog-api-key"), "test-key-not-real")
        self.assertNotIn("test-key", request.full_url)
        tools = json.loads(request.data)["tools"][0]["functionDeclarations"]
        self.assertEqual({tool["name"] for tool in tools}, {"list_files", "read_file"})

    @patch.dict(os.environ, {"GEMINI_API_KEY": "test-key-not-real"})
    @patch("urllib.request.urlopen")
    def test_rate_limit_is_not_retried(self, opener):
        opener.side_effect = HTTPError("https://example.invalid", 429, "secret", {}, None)
        with self.assertRaisesRegex(RuntimeError, "Quota/rate limit"):
            Gemini().ask([], False)
        self.assertEqual(opener.call_count, 1)

    @patch.dict(os.environ, {"GEMINI_API_KEY": "test-key-not-real"})
    @patch("urllib.request.urlopen")
    def test_incomplete_response_cannot_execute(self, opener):
        opener.return_value.__enter__.return_value.read.return_value = json.dumps(
            {"candidates": [{"finishReason": "MAX_TOKENS", "content": reply(call())}]}).encode()
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            Gemini().ask([], False)


if __name__ == "__main__":
    unittest.main()
