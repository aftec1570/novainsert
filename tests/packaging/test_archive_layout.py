"""Release archives (tools/package_archive.sh): name, members and checksum for the Linux tar.gz and the Windows zip.
Run: python3 -m unittest tests/packaging/test_archive_layout.py"""
import hashlib, pathlib, subprocess, tarfile, tempfile, unittest, zipfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/package_archive.sh"

def fake_build(d, tag, binary):
    b = d / "build"; (b / "NovaInsert.vst3/Contents" / binary).parent.mkdir(parents=True)
    (b / "NovaInsert.vst3/Contents" / binary).write_bytes(b"")
    (b / "CMakeCache.txt").write_text(f"CMAKE_PROJECT_VERSION:STATIC=1.2.0\nNI_PLATFORM_TAG:STRING={tag}\n")
    return b

def top(names):
    """Top-level members, directories with a trailing slash (tar lists "dir", zip lists "dir/")."""
    dirs = {n.split("/")[0] for n in names if "/" in n.rstrip("/") or n.endswith("/")}
    return {n.split("/")[0] + ("/" if n.split("/")[0] in dirs else "") for n in names}

class ArchiveLayout(unittest.TestCase):
    def package(self, tag, binary):
        d = pathlib.Path(tempfile.mkdtemp()); out = d / "dist"
        r = subprocess.run(["bash", str(SCRIPT), str(fake_build(d, tag, binary)), str(out)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return out

    def check_sha(self, path):
        want = hashlib.sha256(path.read_bytes()).hexdigest()
        line = (path.parent / (path.name + ".sha256")).read_text().split()
        self.assertEqual(line, [want, path.name])

    def test_linux_tarball(self):
        out = self.package("Linux-x86_64", "x86_64-linux/NovaInsert.so")
        a = out / "NovaInsert-1.2.0-Linux-x86_64.tar.gz"; self.assertTrue(a.exists(), list(out.iterdir()))
        with tarfile.open(a) as t: names = t.getnames()
        self.assertEqual(top(names), {"NovaInsert.vst3/", "README-Linux.txt", "LICENSE.txt", "BUGS.md", "VERSION.md", "install.sh"})
        self.assertIn("NovaInsert.vst3/Contents/x86_64-linux/NovaInsert.so", names)
        with tarfile.open(a) as t: self.assertTrue(t.getmember("install.sh").mode & 0o100, "install.sh not executable")
        self.check_sha(a)

    def test_windows_zip(self):
        out = self.package("Windows-x64", "x86_64-win/NovaInsert.vst3")
        a = out / "NovaInsert-1.2.0-Windows-x64.zip"; self.assertTrue(a.exists(), list(out.iterdir()))
        with zipfile.ZipFile(a) as z: names = z.namelist()
        self.assertEqual(top(names), {"NovaInsert.vst3/", "README-Windows.txt", "LICENSE.txt", "BUGS.md", "VERSION.md"})
        self.assertIn("NovaInsert.vst3/Contents/x86_64-win/NovaInsert.vst3", names)
        self.check_sha(a)

    def test_missing_platform_tag_fails(self):
        d = pathlib.Path(tempfile.mkdtemp()); b = fake_build(d, "Linux-x86_64", "x86_64-linux/NovaInsert.so")
        (b / "CMakeCache.txt").write_text("CMAKE_PROJECT_VERSION:STATIC=1.2.0\n")
        r = subprocess.run(["bash", str(SCRIPT), str(b), str(d / "dist")], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)

if __name__ == "__main__":
    unittest.main()
