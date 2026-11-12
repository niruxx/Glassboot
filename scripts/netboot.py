#!/usr/bin/env python3
"""Prepare and serve a Linux PXE tree using the locally built GRUB tools."""
import argparse
import functools
import hashlib
import http.server
import ipaddress
import json
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PLATFORMS = {"i386-pc": ("i386-pc-pxe", "glassboot.0"),
             "x86_64-efi": ("x86_64-efi", "BOOTX64.EFI"),
             "i386-efi": ("i386-efi", "BOOTIA32.EFI"),
             "arm64-efi": ("arm64-efi", "BOOTAA64.EFI")}


def quote(value):
    if any(ord(c) < 32 for c in value):
        raise ValueError("Control characters are not allowed in menu values")
    return '"' + ''.join('\\' + c if c in '\\"$' else c for c in value) + '"'


def run(args):
    subprocess.run([str(a) for a in args], check=True, stdout=subprocess.DEVNULL)


def extract(iso, source, target):
    with tempfile.TemporaryDirectory(dir=target.parent) as work:
        candidate = Path(work) / 'file'
        result = subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(iso),
                                 '-extract', source, str(candidate)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode or candidate.is_symlink() or not candidate.is_file():
            return False
        shutil.move(str(candidate), str(target))
        return True


def first(iso, paths, target):
    # Debian Live commonly uses versioned regular files plus convenience symlinks.
    folder = str(Path(paths[0]).parent)
    prefix = Path(paths[0]).name
    listing = subprocess.run(['xorriso', '-indev', str(iso), '-ls', folder],
                             capture_output=True, text=True)
    if listing.returncode == 0:
        paths = list(paths) + [folder + '/' + name for name in shlex.split(listing.stdout)
                               if name.startswith(prefix + '-') and '/' not in name]
    for source in paths:
        if extract(iso, source, target):
            return source
    return None


def prepare_image(iso, dest, base):
    """Use distro-supported HTTP media fetching; never run a local-disk hook over PXE."""
    dest.mkdir(parents=True)
    profile = None
    if first(iso, ['/casper/vmlinuz', '/casper/vmlinuz.efi'], dest / 'vmlinuz'):
        if first(iso, ['/casper/initrd', '/casper/initrd.lz', '/casper/initrd.gz', '/casper/initrd.zst'], dest / 'initrd'):
            profile = 'casper'
    elif first(iso, ['/live/vmlinuz', '/live/vmlinuz1', '/live/vmlinuz2'], dest / 'vmlinuz'):
        if first(iso, ['/live/initrd.img', '/live/initrd1.img', '/live/initrd2.img'], dest / 'initrd'):
            if extract(iso, '/live/filesystem.squashfs', dest / 'filesystem.squashfs'):
                profile = 'live-boot'
    if profile == 'casper':
        # Copy, rather than symlink, so the served tree is self-contained.
        shutil.copyfile(iso, dest / 'media.iso')
        args = f'boot=casper ip=dhcp netboot=url iso-url={base}/media.iso url={base}/media.iso'
    elif profile == 'live-boot':
        args = f'boot=live components ip=dhcp fetch={base}/filesystem.squashfs'
    else:
        return None
    device = base.replace('http://', '(http,', 1).replace('/images/', ')/images/', 1)
    return f'linux {quote(device + "/vmlinuz")} {args}\ninitrd {quote(device + "/initrd")}'


def prepare(args):
    source = args.isos.resolve(strict=True)
    dest = args.output.resolve()
    if not source.is_dir():
        raise ValueError('--isos must be a directory')
    if dest == source or source in dest.parents or dest in source.parents:
        raise ValueError('ISO source and output must be separate, non-overlapping directories')
    # A dedicated, new output directory avoids deleting user data or stale artifacts.
    if dest.exists():
        raise ValueError('Output already exists; choose a new directory for each preparation')
    if any(c in str(dest) for c in '\r\n#,\"'):
        raise ValueError('Output path contains characters unsupported by dnsmasq configuration')
    server = str(ipaddress.IPv4Address(args.server))
    if not 1 <= args.port <= 65535:
        raise ValueError('Invalid HTTP port')
    base = f'http://{server}:{args.port}'
    isos = sorted((p for p in source.rglob('*') if p.suffix.lower() == '.iso'
                   and p.is_file() and not p.is_symlink()
                   and source in p.resolve().parents), key=lambda p: p.relative_to(source).as_posix().casefold())
    if not isos:
        raise ValueError('No ISO files found')
    for platform in args.platforms:
        if not (args.grub / 'lib/grub' / platform / 'normal.mod').is_file():
            raise ValueError(f'Missing GRUB platform: {platform}; run scripts/build.sh grub')
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.glassboot-pxe-', dir=dest.parent) as temp:
        stage = Path(temp) / 'tree'
        cfgdir = stage / 'boot/grub'
        cfgdir.mkdir(parents=True)
        menu = ['set timeout=-1', 'set default=0', 'insmod http', 'insmod linux',
                'insmod echo', 'insmod read']
        inventory = []
        for iso in isos:
            name = iso.relative_to(source).as_posix()
            ident = hashlib.sha256(name.encode()).hexdigest()[:20]
            image = stage / 'images' / ident
            body = prepare_image(iso, image, f'{base}/images/{ident}')
            inventory.append({'path': name, 'supported': body is not None})
            if body is None:
                shutil.rmtree(image)
                body = 'echo "This ISO has no supported HTTP boot profile (Casper / Debian Live)."\necho "Press Enter to return."\nread'
                name += ' [unsupported network profile]'
            menu.extend([f'menuentry {quote(name)} {{', body, '}'])
        menu.extend(['menuentry "Reboot" { reboot }'])
        (cfgdir / 'grub.cfg').write_text('\n'.join(menu) + '\n', encoding='utf-8')
        (stage / 'inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
        early = Path(temp) / 'early.cfg'
        early.write_text(f'net_bootp\nset prefix=(http,{server}:{args.port})/boot/grub\nconfigfile $prefix/grub.cfg\n')
        for platform in args.platforms:
            fmt, filename = PLATFORMS[platform]
            modules = args.grub / 'lib/grub' / platform
            shutil.copytree(modules, cfgdir / platform,
                            ignore=shutil.ignore_patterns('*.img', '*.module', '*.o', '*.a'))
            netmods = ['pxe'] if platform == 'i386-pc' else ['efinet']
            run([args.grub / 'bin/grub-mkimage', '-O', fmt, '-d', modules,
                 '-p', f'(http,{server}:{args.port})/boot/grub', '-c', early,
                 '-o', stage / filename, 'normal', 'configfile', 'net',
                 'tftp', 'http', 'linux', 'echo', 'read', 'test', *netmods])
        # DHCP remains on the existing router. This file supplies proxy-DHCP + TFTP.
        lines = ['port=0', f'interface={args.interface}', 'bind-dynamic',
                 f'dhcp-range={server},proxy', 'enable-tftp', f'tftp-root={dest}',
                 'log-dhcp']
        services = {'i386-pc': ('x86PC', 'glassboot'),
                    'x86_64-efi': ('X86-64_EFI', 'BOOTX64.EFI'),
                    'i386-efi': ('IA32_EFI', 'BOOTIA32.EFI'),
                    'arm64-efi': ('ARM64_EFI', 'BOOTAA64.EFI')}
        for platform in args.platforms:
            arch, filename = services[platform]
            lines.append(f'pxe-service={arch},"Glassboot",{filename},{server}')
            if platform == 'x86_64-efi':
                lines.append(f'pxe-service=BC_EFI,"Glassboot",{filename},{server}')
        (stage / 'dnsmasq.conf').write_text('\n'.join(lines) + '\n')
        shutil.move(str(stage), str(dest))
    print(f'Prepared {len(isos)} ISOs ({sum(i["supported"] for i in inventory)} supported) in {dest}')
    print(f'HTTP: python3 scripts/netboot.py serve --root {shlex.quote(str(dest))} --bind {server} --port {args.port}')
    print(f'PXE: sudo dnsmasq --no-daemon --conf-file={shlex.quote(str(dest / "dnsmasq.conf"))}')


class Handler(http.server.SimpleHTTPRequestHandler):
    """Serve only files resolved inside the PXE root; do not follow escaping symlinks."""
    def send_head(self):
        root = Path(self.directory).resolve()
        path = Path(self.translate_path(self.path)).resolve()
        if root not in path.parents or not path.is_file():
            self.send_error(404)
            return None
        return super().send_head()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    prep = sub.add_parser('prepare', help='Extract supported ISO media and build network loaders')
    prep.add_argument('--isos', type=Path, required=True)
    prep.add_argument('--output', type=Path, required=True)
    prep.add_argument('--server', required=True, help='LAN IPv4 address advertised to clients')
    prep.add_argument('--port', type=int, default=8080)
    prep.add_argument('--interface', required=True, help='LAN interface for proxy-DHCP, e.g. enp3s0')
    prep.add_argument('--grub', type=Path, default=ROOT / 'out/grub')
    prep.add_argument('--platforms', nargs='+', choices=PLATFORMS, default=list(PLATFORMS))
    serve = sub.add_parser('serve', help='Serve an already prepared tree over HTTP')
    serve.add_argument('--root', type=Path, required=True)
    serve.add_argument('--bind', default='127.0.0.1')
    serve.add_argument('--port', type=int, default=8080)
    args = parser.parse_args()
    if args.command == 'prepare':
        if not args.interface or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-' for c in args.interface):
            parser.error('Invalid network interface')
        try:
            prepare(args)
        except (ValueError, OSError, subprocess.CalledProcessError) as exc:
            parser.exit(1, f'{exc}\n')
    else:
        root = args.root.resolve(strict=True)
        if not root.is_dir():
            parser.error('--root must be a directory')
        handler = functools.partial(Handler, directory=str(root))
        with http.server.ThreadingHTTPServer((args.bind, args.port), handler) as server:
            print(f'Serving {root} on {args.bind}:{args.port}', flush=True)
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass


if __name__ == '__main__':
    main()
