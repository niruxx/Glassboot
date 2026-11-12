"""Release verifiers must fail closed; ZIP output must ignore source mtimes."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

source = Path(__file__).resolve().parents[2]/'scripts/release.py'
spec = importlib.util.spec_from_file_location('release_tools',source)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def test_payload_and_bad_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = []
            for name in ['mbr.bin','core.img','esp.img']:
                (root/name).write_bytes(name.encode())
                lines.append(f'file {name} {len(name)} {release.digest(root/name)}\n')
            (root/'manifest.txt').write_text(''.join(lines))
            self.assertEqual(len(release.verify_payload(root)),3)
            (root/'core.img').write_bytes(b'changed!')
            with self.assertRaises(ValueError): release.verify_payload(root)
            with self.assertRaises(ValueError): release.safe_file(root,'../outside')
            (root/'manifest.txt').write_text(lines[0]+lines[0])
            with self.assertRaises(ValueError): release.verify_payload(root)

    def test_archive_and_tamper(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)/'portable'; root.mkdir()
            data = root/'test.txt'; data.write_bytes(b'portable file')
            (root/'files.sha256').write_text(release.digest(data)+'  test.txt\n')
            a,b = Path(tmp)/'a.zip',Path(tmp)/'b.zip'
            release.archive(root,a)
            os.utime(data,(2000000000,2000000000))
            release.archive(root,b)
            self.assertEqual(a.read_bytes(),b.read_bytes())
            data.write_bytes(b'corrupt')
            with self.assertRaises(ValueError): release.verify(root)


if __name__ == '__main__': unittest.main()
