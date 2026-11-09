"""The GitHub Actions workflows run only in the private CI repo: tools/github_mirror.sh also publishes .github to the public
source view, where a mirror push or the public release tag must not start Windows/Linux builds (final review 2026-11-09).
Run: python3 -m unittest tests/packaging/test_ci_workflows.py"""
import pathlib, re, unittest
WF = pathlib.Path(__file__).resolve().parents[2] / ".github/workflows"
GUARD = "if: github.repository == 'aftec1570/novainsert-ci'"

def jobs(text):
    """{job name: its block} for the top-level jobs: map (two-space keys)."""
    body = text.split("\njobs:\n", 1)[1]
    parts = re.split(r"(?m)^  ([A-Za-z0-9_-]+):\s*$", body)
    return dict(zip(parts[1::2], parts[2::2]))

class CiWorkflows(unittest.TestCase):
    def test_every_job_is_guarded_to_the_private_ci_repo(self):
        files = sorted(WF.glob("*.yml")); self.assertTrue(files)
        for f in files:
            js = jobs(f.read_text()); self.assertTrue(js, f.name)
            for name, block in js.items():
                self.assertIn("    " + GUARD, block, f"{f.name}: job {name}")

if __name__ == "__main__":
    unittest.main()
