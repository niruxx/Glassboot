#!/usr/bin/env python3
"""Release inventory, verification and deterministic archives; no third-party Python deps."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EPOCH = int(os.environ.get('SOURCE_DATE_EPOCH', '1704067200'))
VERSION = re.search(r'^set glassboot_version="([^"]+)"', (ROOT/'boot/grub/grub.cfg').read_text(encoding='utf-8'), re.M)[1]


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def files(root):
    return sorted((p for p in root.rglob('*') if p.is_file()), key=lambda p: p.relative_to(root).as_posix())


def safe_file(root, name):
    path = (root/name).resolve()
    if path == root.resolve() or root.resolve() not in path.parents:
        raise ValueError('manifest path outside package: '+name)
    return path


def verify_payload(root):
    found = set()
    for line in (root/'manifest.txt').read_text(encoding='utf-8').splitlines():
        if not line.startswith('file '):
            continue
        _, name, size, sha = line.split()
        path = safe_file(root, name)
        if name in found or not path.is_file() or path.stat().st_size != int(size) or digest(path) != sha:
            raise ValueError('payload verification failed: '+name)
        found.add(name)
    if not {'mbr.bin','core.img','esp.img'} <= found:
        raise ValueError('incomplete payload manifest')
    return found


def copy_payload(source, dest):
    names = verify_payload(source)
    dest.mkdir(parents=True)
    for name in sorted(names | {'manifest.txt'}):
        shutil.copy2(source/name, dest/name)


def json_file(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True)+'\n', encoding='utf-8', newline='\n')


def inventory(root, platform):
    contents = [p for p in files(root) if p.name not in {'SBOM.cdx.json','files.sha256','release.json'}]
    components = []
    for p in contents:
        name = p.relative_to(root).as_posix()
        sha = digest(p)
        components.append({'type':'file', 'bom-ref':name, 'name':name,
                           'hashes':[{'alg':'SHA-256','content':sha}]})
    pins = {}
    for line in (ROOT/'versions.env').read_text(encoding='utf-8').splitlines():
        if re.match(r'^[A-Z_]+=\S+$', line):
            key, value = line.split('=',1); pins[key] = value
    for line in (ROOT/'packaging/versions.env').read_text(encoding='utf-8').splitlines():
        if re.match(r'^[A-Z_]+=\S+$', line):
            key, value = line.split('=',1); pins[key] = value
    for name, version in [('GNU GRUB',pins['GRUB_VERSION']),('wimboot',pins['WIMBOOT_TAG']),
                          ('syslinux memdisk',pins['SYSLINUX_VERSION']),
                          ('AppImage runtime',pins['APPIMAGE_RUNTIME_COMMIT']),
                          ('Squashfuse',pins['SQUASHFUSE_VERSION'])]:
        if platform.startswith('windows') and name in {'AppImage runtime','Squashfuse'}: continue
        components.append({'type':'application' if name != 'Squashfuse' else 'library',
                           'bom-ref':'component:'+name,'name':name,'version':version})
    if (root/'qt-version.txt').exists():
        components.append({'type':'framework','bom-ref':'component:Qt','name':'Qt',
                           'version':(root/'qt-version.txt').read_text().strip()})
    try:
        commit = subprocess.check_output(['git','rev-parse','HEAD'], cwd=ROOT, text=True).strip()
        dirty = subprocess.run(['git','-c','core.safecrlf=false','diff','--quiet'], cwd=ROOT, stderr=subprocess.DEVNULL).returncode != 0
    except (OSError, subprocess.CalledProcessError):
        commit, dirty = 'unknown', True
    json_file(root/'release.json', {'version':VERSION, 'platform':platform, 'sourceCommit':commit,
                                   'workingTreeModified':dirty, 'sourceDateEpoch':EPOCH, 'upstreamPins':pins,
                                   'signature':'unsigned; sign SHA256SUMS with a maintainer key before publishing'})
    json_file(root/'SBOM.cdx.json', {'bomFormat':'CycloneDX','specVersion':'1.6','version':1,
        'metadata':{'component':{'type':'application','name':'Glassboot','version':VERSION},
                    'properties':[{'name':'glassboot:platform','value':platform},
                                  {'name':'glassboot:inventory-scope','value':'bundled files; runtime dependency licenses are not inferred'}]},
        'components':components})
    (root/'files.sha256').write_text(''.join(digest(p)+'  '+p.relative_to(root).as_posix()+'\n'
        for p in files(root) if p.name != 'files.sha256'), encoding='utf-8', newline='\n')


def verify(root):
    count = 0
    for line in (root/'files.sha256').read_text(encoding='utf-8').splitlines():
        sha, name = line.split('  ',1)
        if not re.fullmatch('[0-9a-f]{64}', sha) or digest(safe_file(root,name)) != sha:
            raise ValueError('package verification failed: '+name)
        count += 1
    if not count:
        raise ValueError('empty package inventory')
    print('Verified', count, 'bundled files')


def archive(root, output):
    verify(root)
    stamp = datetime.datetime.fromtimestamp(max(EPOCH,315532800), datetime.timezone.utc).timetuple()[:6]
    temp = output.with_suffix(output.suffix+'.tmp')
    with zipfile.ZipFile(temp,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for p in files(root):
            item = zipfile.ZipInfo(root.name+'/'+p.relative_to(root).as_posix(), stamp)
            item.create_system = 3
            item.external_attr = (stat.S_IFREG | (0o755 if p.stat().st_mode & 0o111 else 0o644)) << 16
            item.compress_type = zipfile.ZIP_DEFLATED
            with p.open('rb') as src, z.open(item,'w',force_zip64=True) as dst:
                shutil.copyfileobj(src,dst,1024*1024)
    temp.replace(output)


def esp_manifest(root):
    target = root/'boot/glassboot/esp.sha256'
    target.write_text(''.join(digest(p)+'  '+p.relative_to(root).as_posix()+'\n' for p in files(root)
                              if p != target), encoding='utf-8', newline='\n')


def linux_stage(build, payload, stage):
    if stage.exists():
        raise ValueError('AppDir must not already exist: '+str(stage))
    binary, lib, plugins = stage/'usr/bin', stage/'usr/lib', stage/'usr/plugins'
    binary.mkdir(parents=True); lib.mkdir()
    for name in ['glassboot-installer','glassboot-cli']:
        shutil.copy2(build/name,binary/name)
    qt_plugins = Path(subprocess.check_output(['qmake6','-query','QT_INSTALL_PLUGINS'], text=True).strip())
    for name in ['platforms','imageformats','iconengines','xcbglintegrations','wayland-shell-integration',
                 'wayland-graphics-integration-client']:
        if (qt_plugins/name).is_dir():
            shutil.copytree(qt_plugins/name,plugins/name)
    # Bundle the transitive ELF closure, keeping the host's glibc/loader family.
    excluded = {'libc.so.6','libm.so.6','libdl.so.2','libpthread.so.0','librt.so.1',
                'libresolv.so.2','libutil.so.1','ld-linux-x86-64.so.2'}
    queue = files(binary)+files(plugins)
    seen = set()
    while queue:
        obj = queue.pop()
        if obj in seen: continue
        seen.add(obj)
        text = subprocess.check_output(['ldd',str(obj)], text=True)
        if 'not found' in text: raise ValueError('unresolved runtime dependency: '+text)
        for name, path in re.findall(r'\s*(\S+)\s+=>\s+(/\S+)',text):
            dest = lib/name
            if name not in excluded and not dest.exists():
                shutil.copy2(path,dest); queue.append(dest)
    for p in files(binary)+files(lib)+files(plugins):
        rpath = '$ORIGIN/../lib' if p.parent == binary else os.path.relpath(lib,p.parent)
        if not rpath.startswith('$'): rpath = '$ORIGIN/'+rpath
        subprocess.run(['patchelf','--set-rpath',rpath,str(p)],check=True)
    copy_payload(payload,stage/'usr/share/glassboot/payload')
    (binary/'qt.conf').write_text('[Paths]\nPrefix=..\nPlugins=plugins\nTranslations=translations\n')
    translations = stage/'usr/translations'; translations.mkdir()
    qt_translations = Path(subprocess.check_output(['qmake6','-query','QT_INSTALL_TRANSLATIONS'],text=True).strip())
    for p in qt_translations.glob('qtbase_*.qm'): shutil.copy2(p,translations/p.name)
    (stage/'AppRun').write_text('''#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export QT_PLUGIN_PATH="$HERE/usr/plugins"
if [ "${1:-}" = --cli ]; then shift; exec "$HERE/usr/bin/glassboot-cli" "$@"; fi
exec "$HERE/usr/bin/glassboot-installer" "$@"
''',newline='\n')
    (stage/'AppRun').chmod(0o755)
    shutil.copy2(ROOT/'packaging/glassboot.desktop',stage/'glassboot.desktop')
    shutil.copy2(ROOT/'packaging/glassboot.svg',stage/'glassboot.svg')
    shutil.copy2(ROOT/'packaging/NOTICES.txt',stage/'NOTICES.txt')
    shutil.copy2(ROOT/'packaging/PORTABLE-README.txt',stage/'README.txt')
    licenses = stage/'LICENSES'; licenses.mkdir()
    for name in ['GPL-2','GPL-3','LGPL-3']:
        shutil.copy2(ROOT/'packaging/licenses'/(name+'.txt'),licenses/(name+'.txt'))
    (stage/'qt-version.txt').write_text(subprocess.check_output(['qmake6','-query','QT_VERSION'],text=True))
    inventory(stage,'linux-x86_64')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='cmd',required=True)
    for name in ['esp-manifest','verify','verify-payload']:
        p = sub.add_parser(name); p.add_argument('root',type=Path)
    p = sub.add_parser('inventory'); p.add_argument('root',type=Path); p.add_argument('platform')
    p = sub.add_parser('zip'); p.add_argument('root',type=Path); p.add_argument('output',type=Path)
    p = sub.add_parser('linux-stage'); p.add_argument('build',type=Path); p.add_argument('payload',type=Path); p.add_argument('stage',type=Path)
    p = sub.add_parser('hashes'); p.add_argument('dist',type=Path)
    sub.add_parser('version')
    args = parser.parse_args()
    if args.cmd == 'version': print(VERSION)
    elif args.cmd == 'esp-manifest': esp_manifest(args.root)
    elif args.cmd == 'verify': verify(args.root)
    elif args.cmd == 'verify-payload': print('Verified payload:', ', '.join(sorted(verify_payload(args.root))))
    elif args.cmd == 'inventory': inventory(args.root,args.platform)
    elif args.cmd == 'zip': archive(args.root,args.output)
    elif args.cmd == 'linux-stage': linux_stage(args.build,args.payload,args.stage)
    elif args.cmd == 'hashes':
        artifacts = sorted(p for p in args.dist.iterdir() if p.is_file() and (p.suffix in {'.AppImage','.zip','.json'}))
        (args.dist/'SHA256SUMS').write_text(''.join(digest(p)+'  '+p.name+'\n' for p in artifacts),newline='\n')


if __name__ == '__main__':
    main()
