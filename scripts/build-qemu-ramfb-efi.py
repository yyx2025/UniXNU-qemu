#!/usr/bin/env python3
"""Build package/QemuRamfbPkg as a standalone driver image, build/qemu-ramfb-efi/QemuRamfb.efi.

run-qemu.py loads it before DarwinBoot: it switches QEMU's ramfb to the size of the fw_cfg item
opt/org.unixnu/ramfb-size. Run in the build tree after "unixnu build package", which leaves the edk2 reference and
the CLANGPDB tool links (../toolchain), as build-darwinboot-efi.py does.
"""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
toolchain = ROOT.parent / 'toolchain'
work = ROOT / 'build/qemu-ramfb-efi'
candidates = ([Path(os.environ['UNIXNU_EDK2'])] if os.environ.get('UNIXNU_EDK2') else []) + \
    [ROOT / 'refer/edk2'] + sorted(ROOT.parents[1].glob('feeds/mu_basecore-*'))
edk2 = next((path.resolve() for path in candidates if (path / 'MdePkg/MdePkg.dec').is_file()), None)
if edk2 is None:
    raise SystemExit('no edk2 tree (UNIXNU_EDK2, refer/edk2 or out/feeds); run "unixnu build package" first')
if not (toolchain / 'lld-link').exists():
    raise SystemExit(f'{toolchain} has no CLANGPDB tools; run "unixnu build package" first')
tools = edk2 / 'BaseTools'
if not (tools / 'Source/C/bin/GenFw').is_file():
    subprocess.run(['make', '-C', tools / 'Source/C', f'-j{os.cpu_count() or 4}'], check=True)
conf = work / 'Conf'
conf.mkdir(parents=True, exist_ok=True)
for name in ('target', 'tools_def', 'build_rule'):
    if not (conf / f'{name}.txt').is_file():
        shutil.copy(tools / 'Conf' / f'{name}.template', conf / f'{name}.txt')
# The board's package directory is kits/qemu in the build tree.
env = dict(os.environ, WORKSPACE=str(work), CONF_PATH=str(conf), EDK_TOOLS_PATH=str(tools), PYTHON_COMMAND='python3',
           PACKAGES_PATH=':'.join(str(p) for p in (ROOT / 'kits/qemu', edk2)),
           CLANG_BIN=str(toolchain) + '/',
           PATH=os.pathsep.join([str(tools / 'BinWrappers/PosixLike'), str(tools / 'Source/C/bin'), os.environ['PATH']]))
subprocess.run(['build', '-p', 'QemuRamfbPkg/QemuRamfbPkg.dsc', '-a', 'AARCH64', '-b', 'DEBUG', '-t', 'CLANGPDB',
                '-n', str(os.cpu_count() or 4)], cwd=work, env=env, check=True, stdout=subprocess.DEVNULL)
built = work / 'Build/QemuRamfb/DEBUG_CLANGPDB/AARCH64/QemuRamfb.efi'
shutil.copyfile(built, work / 'QemuRamfb.efi')
print(work / 'QemuRamfb.efi')
