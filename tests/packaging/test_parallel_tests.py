"""Every unit-test runner uses all CPU cores: ctest runs with -j/--parallel and run_unit.sh runs its presets side by side
(user request 2026-11-09). Real-time REAPER/audio runs (tier B, f3, apollo) stay serial and are not covered here.
Run: python3 -m unittest tests/packaging/test_parallel_tests.py"""
import pathlib, re, unittest
ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNNERS = ["tests/run_unit.sh", "tests/linux/run_tier_a.sh", ".github/workflows/ci.yml"]

def ctest_lines(text):
    return [l for l in text.splitlines() if re.search(r"\bctest\b", l) and not l.lstrip().startswith("#")]

class ParallelTests(unittest.TestCase):
    def test_every_ctest_run_is_parallel(self):
        for rel in RUNNERS:
            lines = ctest_lines((ROOT / rel).read_text()); self.assertTrue(lines, rel)
            for l in lines:
                self.assertRegex(l, r"ctest\b.*(\s-j|\s--parallel)", f"{rel}: {l.strip()}")

    def test_run_unit_runs_presets_concurrently(self):
        text = (ROOT / "tests/run_unit.sh").read_text()
        self.assertRegex(text, r"(?m)^\s*run_preset .*&\s*$")
        self.assertRegex(text, r"(?m)^\s*wait\b")

if __name__ == "__main__":
    unittest.main()
