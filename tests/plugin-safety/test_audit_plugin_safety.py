import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "audit-plugin-safety.py"
SPEC = importlib.util.spec_from_file_location("audit_plugin_safety", SCRIPT)
AUDIT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = AUDIT
SPEC.loader.exec_module(AUDIT)


class AuditPluginSafetyTests(unittest.TestCase):
    def rules(self, source):
        return [finding.rule for finding in AUDIT.audit_text(source, "fixture.cpp")]

    def test_masks_comments_and_strings(self):
        self.assertEqual([], self.rules('const char *s = "new int[count]"; // new int[count]\n'))

    def test_rejects_local_integer_and_widening_hazards(self):
        source = """
void f(tjs_int param) {
  tjs_uint count = param;
  auto bytes = (tjs_int64)(width * height);
}
"""
        self.assertEqual(["KPS101", "KPS204", "KPS102"], self.rules(source))

    def test_allocation_requires_visible_chain(self):
        unsafe = "void f(tTJSVariant **param) { size_t count = (tjs_int)*param[0]; auto p = new int[count]; }"
        safe = """
void f(tTJSVariant **param) {
  pluginSafety::OperationBudget budget;
  auto bytes = pluginSafety::checkedElementBytes(count, sizeof(int));
  auto allocation = pluginSafety::validateAllocationBudget(bytes.value, budget);
  auto p = new int[allocation.value.elementCount()];
}
"""
        self.assertIn("KPS201", self.rules(unsafe))
        self.assertNotIn("KPS201", self.rules(safe))

    def test_reassignment_and_cross_function_are_not_inferred(self):
        cross_function = """
ValidatedAllocation validate(size_t count);
void f(tTJSVariant **param) { size_t count = (tjs_int)*param[0]; auto allocation = validate(count); auto p = new int[count]; }
"""
        self.assertIn("KPS201", self.rules(cross_function))
        reassigned = """
void f(tTJSVariant **param) {
  size_t count = (tjs_int)*param[0];
  OperationBudget budget;
  auto bytes = checkedElementBytes(count, sizeof(int));
  auto allocation = validateAllocationBudget(bytes.value, budget);
  count = uncheckedReplacement;
  auto p = new int[count];
}
"""
        self.assertIn("KPS201", self.rules(reassigned))

    def test_unbalanced_parentheses_are_tool_errors(self):
        with self.assertRaisesRegex(ValueError, "parenthesis"):
            AUDIT.audit_text("void f( { }", "fixture.cpp")

    def test_layer_and_tjs_numeric_rules(self):
        source = """
void f(iTJSDispatch2 *layer, tTJSVariant tmp) {
  layer->PropGet(0, TJS_W("mainImageBuffer"), 0, &tmp, layer);
  duration = (tjs_int)tmp;
}
"""
        self.assertIn("KPS103", self.rules(source))
        self.assertIn("KPS204", self.rules(source))

    def test_smoke_pass_cannot_follow_stop_or_delay(self):
        source = 'stopTransition();\nSystem.inform("PLUGIN_SMOKE_PASS plugin=x.dll");\n'
        self.assertIn("KPS104", self.rules(source))
        self.assertIn(
            "KPS104",
            self.rules("System.delay(100);\npluginSmokePass(\"x.dll\");\n"),
        )

    def test_multiple_budgets_in_one_block_are_rejected(self):
        source = "void f() { OperationBudget a; OperationBudget b; }"
        self.assertIn("KPS205", self.rules(source))

    def test_narrow_waiver_requires_specific_reason_and_exact_rule(self):
        valid = """
void f(tTJSVariant **param) {
// plugin-safety: allow KPS201 -- count <= validated header.frameCount <= 4096
size_t count = (tjs_int)*param[0]; auto p = new int[count];
}
"""
        invalid = """
void f(size_t count) {
// plugin-safety: allow KPS201 -- safe here
size_t count = (tjs_int)*param[0]; auto p = new int[count];
}
"""
        self.assertNotIn("KPS201", self.rules(valid))
        self.assertIn("KPS091", self.rules(invalid))

    def test_stale_and_mismatched_waivers_fail(self):
        stale = "// plugin-safety: allow KPS201 -- fixed capacity 32\nint x = 1;\n"
        mismatch = "void f(tTJSVariant **param) {\n// plugin-safety: allow KPS202 -- fixed capacity 32\nsize_t count = (tjs_int)*param[0]; auto p = new int[count];\n}\n"
        self.assertIn("KPS090", self.rules(stale))
        self.assertIn("KPS092", self.rules(mismatch))

    def test_cli_json_and_exit_codes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            plugins = root / "cpp" / "plugins"
            plugins.mkdir(parents=True)
            (plugins / "bad.cpp").write_text(
                "void f(tTJSVariant **param){size_t count=(tjs_int)*param[0];new int[count];}",
                encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--root", str(root), "--format", "json"],
                text=True, capture_output=True, check=False)
            self.assertEqual(1, result.returncode)
            self.assertIn("KPS201", [item["rule"] for item in
                                      json.loads(result.stdout)["findings"]])
            config_error = subprocess.run(
                [sys.executable, str(SCRIPT), "--root", str(root / "missing")],
                text=True, capture_output=True, check=False)
            self.assertEqual(2, config_error.returncode)


if __name__ == "__main__":
    unittest.main()
