#!/usr/bin/env python3
"""Check that a run-qemu.py run reached the expected stage.

run-qemu.py prints its observations to stdout ("native user: PC ...") and writes
the serial log to the run directory. This script reads both and exits non-zero
when the stage did not happen, so CI fails instead of silently passing.
"""
import argparse
import json
import re
import sys
from pathlib import Path


def latest_run(work):
    runs = sorted((work / 'run').glob('*/'), key=lambda p: p.name)
    return runs[-1] if runs else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True, help='the build work dir (out/<board>)')
    parser.add_argument('--stdout', type=Path, help='captured stdout of run-qemu.py')
    parser.add_argument('--expect-native-user', action='store_true',
                        help='require the "native user: PC ..." checkpoint line')
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

    native = run / 'native-user.txt'
    if native.is_file():
        print('--- native-user.txt ---')
        print(native.read_text(errors='replace'))

    screenshots = sorted(run.glob('screen-*.png'))
    print(f'screenshots: {len(screenshots)}')

    failures = []
    if args.expect_native_user:
        match = re.search(r'native user: PC ([0-9a-fA-F]+).*?EL(\d).*?getpid (\S+).*?hw\.pagesize (\S+)',
                          console, re.S)
        if not match:
            failures.append('no "native user:" checkpoint in the run output')
        else:
            pc, level, pid, page = match.groups()
            print(f'native user: PC {pc}, EL{level}, getpid {pid}, hw.pagesize {page}')
            if level != '0':
                failures.append(f'the test process is at EL{level}, not EL0')
            if pid != '1':
                failures.append(f'getpid returned {pid}, not 1')
            if page != '4096':
                failures.append(f'hw.pagesize is {page}, not 4096')
    if args.expect_screen and not screenshots:
        failures.append('no screenshot was taken')
    if args.reject_panic and re.search(r'panic\(|panic: |Debugger called', serial):
        failures.append('the kernel panicked')

    summary = {'run': run.name, 'screenshots': len(screenshots),
               'failures': failures}
    if 'match' in dir() and match:
        summary.update(native_user=dict(pc=match.group(1), el=int(match.group(2)),
                                        getpid=match.group(3), pagesize=match.group(4)))
    (run / 'check.json').write_text(json.dumps(summary, indent=2) + '\n')
    if failures:
        for failure in failures:
            print(f'FAIL: {failure}', file=sys.stderr)
        return 1
    print('PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
