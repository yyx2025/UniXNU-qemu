#!/usr/bin/env python3
"""Build DarwinBoot as a standalone driver image, build/darwinboot-efi/DarwinBoot.efi.

QEMU's EDK II firmware has no DarwinBoot, so the payload loads it as a driver. Run in the build tree after
"unixnu build package", which leaves the edk2 reference (refer/edk2) and the CLANGPDB tool links (../toolchain).
"""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
toolchain = ROOT.parent / 'toolchain'
work = ROOT / 'build/darwinboot-efi'
# refer/ of the tree only holds the references of the last step, so also look in the driver's feed clones.
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
env = dict(os.environ, WORKSPACE=str(work), CONF_PATH=str(conf), EDK_TOOLS_PATH=str(tools), PYTHON_COMMAND='python3',
           PACKAGES_PATH=':'.join(str(p) for p in (ROOT / 'drivers/DarwinBoot', ROOT / 'package', edk2)),
           CLANG_BIN=str(toolchain) + '/',
           PATH=os.pathsep.join([str(tools / 'BinWrappers/PosixLike'), str(tools / 'Source/C/bin'), os.environ['PATH']]))
subprocess.run(['build', '-p', 'DarwinBootPkg/DarwinBoot.dsc', '-a', 'AARCH64', '-b', 'DEBUG', '-t', 'CLANGPDB',
                '-n', str(os.cpu_count() or 4)], cwd=work, env=env, check=True, stdout=subprocess.DEVNULL)
built = work / 'Build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi'
shutil.copyfile(built, work / 'DarwinBoot.efi')
print(work / 'DarwinBoot.efi')
