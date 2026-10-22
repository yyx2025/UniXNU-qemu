#!/usr/bin/env python3
"""Qualify the original block storage ABI on the QEMU board: build/storage-abi/interface-audit.json.

UniXNU's audit-storage-interface.py reads three inputs that no UniXNU script produces (UniXNU#7). This script makes
them from this board's own kernel, then runs the audit unchanged:

- The candidate: the kernel step's kernel.macho and its prelink plan, under the name the audit reads
  (build/native-prelink/native-apfs-load-positive.*). The original kexts sit at the same addresses in every kernel
  of this IPSW, so bindings taken from it hold for later kernels.
- The live observation: gdb-stop.py boots the candidate from the payload with offline-apfs-load=1, which loads APFS
  and with it IOStorageFamily, stops at native_apfs_probe_checkpoint and reads the two metaclasses and the
  translation registers (build/darwinboot-storage-native-metadata/gdb-stop.json).
- The metaclass layout: the OSMetaClass member offsets from the DWARF of the kernel's own OSMetaClass.cpo, derived
  as audit-aes-interface.py does. That audit needs a macOS KDK, so its output file is written here with the layout
  only, for the storage audit, and removed afterwards: build-native-aes-bindings.py would bind from it instead of
  from its recorded result.

Run in the build tree after "unixnu build kernel" and "unixnu build payload". Takes a few minutes under TCG.
"""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from qemu_esp import ROOT, WORK, stage

AUDIT = ROOT / 'scripts/audit-storage-interface.py'
kernel = WORK / 'kernel/kernel.macho'
if not kernel.is_file():
    raise SystemExit(f'{kernel} missing (unixnu build kernel)')
refer = ROOT / 'refer/IOStorageFamily'
if not refer.is_dir():
    # unixnu links only the references its steps name; this board's feeds.toml adds IOStorageFamily.
    fetched = sorted((WORK.parent / 'feeds').glob('IOStorageFamily-*'))
    if len(fetched) != 1:
        raise SystemExit('IOStorageFamily not fetched: run "unixnu fetch" with this device repository')
    refer.parent.mkdir(exist_ok=True)
    refer.symlink_to(fetched[0])

candidate = ROOT / 'build/native-prelink/native-apfs-load-positive.macho'
candidate.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(kernel, candidate)
shutil.copyfile(kernel.with_suffix('.json'), candidate.with_suffix('.json'))

# The two metaclass addresses to read come from the audit's own static part (everything before the live
# observation): the reference object, the candidate provenance and the original class dispatch.
text = AUDIT.read_text()
marker = "d = json.loads((ROOT / 'build/darwinboot-storage-native-metadata/gdb-stop.json')"
if text.count(marker) != 1:
    raise SystemExit('audit-storage-interface.py changed; update the static part this script runs')
static = {'__file__': str(AUDIT), '__name__': 'storage_audit_static'}
exec(compile(text[:text.index(marker)], str(AUDIT), 'exec'), static)
meta, service_meta = static['meta'], static['service_meta']

symbols = subprocess.check_output(['xcrun', 'nm', candidate], text=True)


def symbol(name):
    found = re.findall(rf'^([0-9a-f]{{16}}) \w {re.escape(name)}$', symbols, re.M)
    if len(found) != 1:
        raise SystemExit(f'{name} not found once in the candidate')
    return int(found[0], 16)


checkpoint = symbol('_native_apfs_probe_checkpoint')
status = symbol('_native_apfs_load_status')

out = ROOT / 'build/darwinboot-storage-native-metadata'
esp = out / 'esp'
stage(esp, candidate, 'offline-apfs-load=1')

requests = [f'{meta:x}:50', f'{service_meta:x}:50', f'{status:x}:4']
# 8 GiB: the 4 GiB DarwinBoot window does not fit in the 4 GiB size below it, and gdb-stop.py offers no 6 GiB.
subprocess.run([sys.executable, ROOT / 'scripts/qemu-entry/gdb-stop.py', hex(checkpoint), '--esp', esp,
                '--kernel', candidate, '--fixture', out, '--qemu-ramfb', '--gic-version', '3',
                '--qemu-memory-mib', '8192', '--system-registers', '--timeout', '900',
                *[part for request in requests for part in ('--read-memory', request)]], check=True)
report = json.loads((out / 'gdb-stop.json').read_text())
load = int.from_bytes(bytes.fromhex(report['memory'][requests[2]]), 'little', signed=True)
if int(report['pc'], 16) != checkpoint or load != 0:
    raise SystemExit(f'APFS was not loaded at the checkpoint (pc {report["pc"]}, OSKextLoadKextWithIdentifier {load})')

dwarf = subprocess.check_output(['xcrun', 'dwarfdump', '--name=OSMetaClass', '--show-children',
                                 ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/libkern/DEVELOPMENT/OSMetaClass.cpo'],
                                text=True)
layout = {}
for member in dwarf.split('DW_TAG_member')[1:]:
    name = re.search(r'DW_AT_name\s+\("([^"]+)"\)', member)
    position = re.search(r'DW_AT_data_member_location\s+\((0x[0-9a-f]+)\)', member)
    if name and position and name[1] in ('superClassLink', 'className', 'classSize'):
        layout[name[1]] = int(position[1], 16)
aes = ROOT / 'build/image4-ignition/aes-interface-audit.json'
if aes.exists():
    raise SystemExit(f'{aes} exists; this script writes that file only temporarily')
aes.parent.mkdir(parents=True, exist_ok=True)
aes.write_text(json.dumps(dict(compiled_metaclass_offsets=layout,
                               partial='metaclass layout only, for audit-storage-interface.py (build-storage-abi.py)')))
try:
    subprocess.run([sys.executable, AUDIT], check=True)
finally:
    aes.unlink()
