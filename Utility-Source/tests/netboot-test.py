#!/usr/bin/env python3
"""Integration checks for PXE preparation, real GRUB loaders, and HTTP containment."""
import functools
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('netboot', ROOT / 'scripts/netboot.py')
netboot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(netboot)


@unittest.skipUnless(shutil.which('xorriso'), 'requires xorriso and built GRUB')
class NetbootTest(unittest.TestCase):
    def test_prepare_and_serve(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            source = work / 'isos'
            source.mkdir()
            for name, folder in [('A "quoted" $name.iso', 'casper'), ('B.iso', 'live'), ('Other.iso', 'other')]:
                tree = work / folder
                (tree / folder).mkdir(parents=True)
                for filename in ['vmlinuz', 'initrd' if folder == 'casper' else 'initrd.img', 'filesystem.squashfs']:
                    (tree / folder / filename).write_bytes(b'fixture:' + filename.encode())
                if folder == 'live':
                    for filename in ['vmlinuz', 'initrd.img']:
                        (tree / folder / filename).rename(tree / folder / (filename + '-test'))
                        (tree / folder / filename).symlink_to(filename + '-test')
                subprocess.run(['xorriso', '-as', 'mkisofs', '-quiet', '-o', str(source / name), str(tree)],
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            output = work / 'pxe'
            command = ['python3', str(ROOT / 'scripts/netboot.py'), 'prepare', '--isos', str(source),
                       '--output', str(output), '--server', '192.0.2.1', '--interface', 'eth0']
            subprocess.run(command, check=True)
            if shutil.which('dnsmasq'):
                subprocess.run(['dnsmasq', '--test', '--conf-file=' + str(output / 'dnsmasq.conf')], check=True)
            menu = (output / 'boot/grub/grub.cfg').read_text()
            self.assertIn('iso-url=http://192.0.2.1:8080/images/', menu)
            self.assertIn('fetch=http://192.0.2.1:8080/images/', menu)
            self.assertIn('linux "(http,192.0.2.1:8080)/images/', menu)
            self.assertIn('A \\"quoted\\" \\$name.iso', menu)
            self.assertIn('unsupported network profile', menu)
            self.assertEqual([i['supported'] for i in json.loads((output / 'inventory.json').read_text())], [True, True, False])
            for _, filename in netboot.PLATFORMS.values():
                self.assertGreater((output / filename).stat().st_size, 1000)
            subprocess.run([str(ROOT / 'out/grub/bin/grub-script-check'), str(output / 'boot/grub/grub.cfg')], check=True)
            # Preparation must never overwrite an existing tree.
            self.assertNotEqual(subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode, 0)
            secret = work / 'outside.txt'
            secret.write_text('not served')
            (output / 'escape').symlink_to(secret)
            handler = functools.partial(netboot.Handler, directory=str(output))
            with netboot.http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler) as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    base = f'http://127.0.0.1:{server.server_port}'
                    with urllib.request.urlopen(base + '/boot/grub/grub.cfg') as response:
                        self.assertEqual(response.read().decode(), menu)
                    for path in ['/escape', '/../outside.txt', '/%2e%2e/outside.txt', '/']:
                        with self.assertRaises(urllib.error.HTTPError) as error:
                            urllib.request.urlopen(base + path)
                        self.assertEqual(error.exception.code, 404)
                finally:
                    server.shutdown()
                    thread.join()


if __name__ == '__main__':
    unittest.main()
