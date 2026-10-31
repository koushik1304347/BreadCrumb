"""Boundary tests; run with python3 -m unittest discover -s agent -v."""
import unittest

from agent import action, plan, validate


class ValidationTests(unittest.TestCase):
    def test_read_only_requests(self):
        for request in ("list", "read report.txt", "summarize /demo/inbox.txt"):
            validate(plan(request))

    def test_guest_path_escape_and_shell_syntax(self):
        for path in ("/README", "/demo/../README", "/demo/a;rm", "/demo/a\nrm",
                     "/demo/sub/file", "/demo/abcdefghijklmnop"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                validate(action("read_file", path=path))

    def test_schema(self):
        for bad in (None, {}, action("unknown"), action("read_file", path=3),
                    action("list_files", extra="x"), {"tool": [], "args": {}}):
            with self.subTest(action=bad), self.assertRaises(ValueError):
                validate(bad)

    def test_effect_requires_demo(self):
        request = action("delete_file", path="/demo/important.txt")
        with self.assertRaises(ValueError):
            validate(request)
        validate(request, destructive=True)

    def test_demo_text_cannot_inject_commands(self):
        for text in ("x; rm /demo/a", "$(id)", "`id`", "a\nb", "x > /a"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                validate(action("print_text", text=text), destructive=True)


if __name__ == "__main__":
    unittest.main()
