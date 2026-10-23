# QEMU bring-up state

Stage 1 is green in CI: `qemu_defconfig` boots to a real EL0 process under the 4 KiB user ABI.
The workflow `.github/workflows/qemu-bringup.yml` builds and runs it on every push.

## What runs

```
EDK II AArch64 firmware
  → UniXNU (menu, 1376x1024 ramfb)
  → DarwinBoot as a driver, EL1
  → kernel entry, MMU on, 4 KiB page tables
  → platform expert, IOKit, AMFI (PMGR bridge)
  → mockfs root on md0
  → UniXNU's static test process as /sbin/launchd at EL0, PID 1
```

Recorded in `out/qemu/run/<time>/`:

| Evidence | File |
| --- | --- |
| firmware → menu → DarwinBoot → kernel hand-off | `serial.log` |
| `CurrentEL 1`, `TCR_EL1` with the 4 KiB granule | `serial.log` |
| the three entry-probe bars on the frame buffer | `screen-*.png` |
| `getpid 1`, `sysctl 0`, `hw.pagesize 0x1000` | `native-user.txt` |
| the pass/fail decision | `check.json` |

`serial.log` of the recorded passing run ends with:

```
UNIXNU: menu: timeout, booting default 0
UNIXNU: boot: entry 'xnu' (QEMU virt: source 4K XNU)
DARWINBOOT: ARM64 va=0xFFFFFFF00427C000 pa=0x4807C000 args=0x4C378000 entry=0x4B1E9488 owned=0x100000000
DARWINBOOT: inputs validated; exiting boot services
DARWINBOOT: hand-off to the kernel
CurrentEL 1  SCTLR_EL1 0x30D0198D  TCR_EL1 0x280803518
ID_AA64MMFR0_EL1 0x22200101122  (PARange 2, 4 KiB granule supported)
```

and `native-user.txt` holds the two reads that prove the process actually ran:

```
0000000100004000: 0x0000000000000001 0x0000000000000000   # getpid 1, sysctl 0
0000000100004028: 0x0000000000001000                       # hw.pagesize 4096
```

The CPU is at EL1 in that snapshot because the test process loops and QEMU is stopped from the monitor;
that is the kernel picking the run back up, not a failure. The recorded user-side values are the proof.

## What this stage does not cover

`qemu_defconfig` stops at a memory root: there is no storage driver, no OS volume and no iPadOS userland.
That is stage 2 (`qemu_root_defconfig` plus `scripts/build-os-root.py`), which needs the storage ABI from
`scripts/build-storage-abi.py`. Stage 1 runs that kernel once in QEMU to record it, which is why the two
stages share the same build.

## Fixes this board needed

The QEMU defconfigs never opted in to UniXNU's archive builders, and three scripts assumed another tree:

1. `CONFIG_XNU_BUILD_ARCHIVES=y` — without it `build_archives()` returns immediately and the kernel link
   fails on `_mockfs_mountroot`, `_mockfs_vfsroot`, `_sheng_pmgr_run` and `_native_legacy_display_probe`,
   all of which the mockfs fixture, the PMGR bridge and the link stubs define.
2. `scripts/build-arm-driver.py` gained `-Wno-unknown-warning-option`; newer clang rejects
   `-Wno-unnecessary-virtual-specifier` under `-Werror`.
3. `scripts/patch-xnu-entry-probe.py` accepts ARM64 NOP padding (a prelinked kernel built here fills the
   tail of `__TEXT_EXEC` with `1f2003d5` rather than zeros), compares whole words when the panic screen
   image is not a multiple of four bytes, and names the words it found when it refuses.
4. The payload step needs `--uefi`, not just `UNIXNU_UEFI`: `unixnu` passes `UNIXNU_UEFI=str(ctx.uefi or '')`
   to the probe patcher, and an empty value sends it looking for `Font.h` in `<tree>/uefi-sheng`.
5. `scripts/run-qemu.py` retries the `_results` read until the user mapping is visible instead of recording a
   single "Cannot access memory" under TCG.

## Next

Stage 2 is the iPadOS userland (`--os-root`). It needs `scripts/build-os-root.py` to complete, which in turn
needs `build-storage-abi.py` — the block storage ABI that stage 1's kernel is booted once to record. The
upstream audit behind it (`audit-storage-interface.py`) reads three inputs that no UniXNU script produces;
`build-storage-abi.py` is this repository's answer, and it runs entirely in QEMU.
