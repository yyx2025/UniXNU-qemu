#!/usr/bin/env python3
"""Check that a run-qemu.py run reached the expected stage.

run-qemu.py writes the serial log, the screenshots and, with --native-user,
native-user.txt into the run directory, and prints a summary line to stdout.

The authoritative evidence for the native user process is native-user.txt: it
holds "info registers" (so the current PC and EL) plus two memory reads, the
results block at _results and the page size at _user_page_size. That file is
written before QEMU is told to quit, so it names the state the process reached
even though the CPU may be back in the kernel by the time it is taken - being in
EL1 there is normal and is not a failure. What matters is that the process
recorded getpid 1 and hw.pagesize 0x1000, which only happens if it ran at EL0
under the 4 KiB user ABI.
"""
import argparse
import json
import re
import sys
from pathlib import Path


def latest_run(work):
    runs = sorted((work / 'run').glob('*/'), key=lambda p: p.name)
    return runs[-1] if runs else None


def read_words(text):
    """The 16-hex-digit words of an HMP 'x /Ngx' answer."""
    return [int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{16})', text)]


def parse_native_user(path):
    """(pc, el, getpid, sysctl_status, pagesize) from native-user.txt, or None."""
    if not path.is_file():
        return None
    text = path.read_text(errors='replace')
    pc = re.search(r'\bPC=([0-9a-fA-F]+)', text)
    level = re.search(r'\bEL(\d)[th]\b', text)
    # The file is four lines of "x /Ngx" output: registers, then _results, then
    # _user_page_size. Split on the blank line that separates the register dump.
    blocks = [block for block in text.split('\n\n') if block.strip()]
    results = []
    pagesize = None
    for block in blocks:
        if ':' not in block:
            continue
        address = block.split(':', 1)[0].strip()
        words = read_words(block)
        if not words:
            continue
        # _user_page_size is a single word; _results is the getpid/sysctl pair.
        if len(words) == 1:
            pagesize = words[0]
        elif len(words) >= 2:
            results = words[:2]
    return dict(pc=int(pc.group(1), 16) if pc else None,
                el=int(level.group(1)) if level else None,
                getpid=results[0] if results else None,
                sysctl_status=results[1] if len(results) > 1 else None,
                pagesize=pagesize)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True, help='the build work dir (out/<board>)')
    parser.add_argument('--stdout', type=Path, help='captured stdout of run-qemu.py')
    parser.add_argument('--expect-native-user', action='store_true',
                        help='require the native user process to have run at EL0 with a 4 KiB page size')
    parser.add_argument('--expect-screen', action='store_true', help='require at least one screenshot')
    parser.add_argument('--reject-panic', action='store_true', help='fail if the serial log shows a panic')
    args = parser.parse_args()

    run = latest_run(args.work)
    if run is None:
        print(f'FAIL: no run directory under {args.work / "run"}', file=sys.stderr)
        return 1
    print(f'run: {run}')

    console = ''
    if args.stdout and args.stdout.is_file():
        console = args.stdout.read_text(errors='replace')
        print('--- run-qemu stdout tail ---')
        print('\n'.join(console.splitlines()[-25:]))

    serial = ''
    log = run / 'serial.log'
    if log.is_file():
        serial = log.read_text(errors='replace')
        print('--- serial tail ---')
        print('\n'.join(serial.splitlines()[-40:]))

    native = parse_native_user(run / 'native-user.txt')
    if native:
        print('--- native user ---')
        print(json.dumps({key: (hex(value) if isinstance(value, int) else value)
                          for key, value in native.items()}, indent=2))

    screenshots = sorted(run.glob('screen-*.png'))
    print(f'screenshots: {len(screenshots)}')

    failures = []
    if args.expect_native_user:
        if native is None:
            failures.append('no native-user.txt: run-qemu.py did not reach the --native-user checkpoint')
        else:
            if native['getpid'] != 1:
                failures.append(f'getpid returned {native["getpid"]}, not 1')
            if native['pagesize'] != 0x1000:
                failures.append(f'hw.pagesize is {native["pagesize"]}, not 4096')
            if native['sysctl_status'] not in (0, None):
                failures.append(f'sysctl returned {native["sysctl_status"]}, not 0')
    if args.expect_screen and not screenshots:
        failures.append('no screenshot was taken')
    if args.reject_panic and re.search(r'panic\(|panic: |Debugger called', serial):
        failures.append('the kernel panicked')

    summary = {'run': run.name, 'screenshots': len(screenshots),
               'native_user': {key: (hex(value) if isinstance(value, int) else value)
                               for key, value in (native or {}).items()},
               'failures': failures}
    (run / 'check.json').write_text(json.dumps(summary, indent=2) + '\n')
    if failures:
        for failure in failures:
            print(f'FAIL: {failure}', file=sys.stderr)
        return 1
    print('PASS: the chain reached EL0 with a 4 KiB page size')
    return 0


if __name__ == '__main__':
    sys.exit(main())
