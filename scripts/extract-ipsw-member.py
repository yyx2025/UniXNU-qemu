#!/usr/bin/env python3
"""Extract a member of the board IPSW that unixnu's component fetch does not cover.

scripts/build-original-system-trust.py reads the OS volume's companion trust cache from
out/ipsw/<build>/<dmg>.trustcache. That file is a zip member of the IPSW under Firmware/,
but it is not one of the manifest components "unixnu ipsw fetch" extracts, so the step
that needs it failed with FileNotFoundError.

  extract-ipsw-member.py IPSW MEMBER PREFIX

MEMBER is a suffix such as 044-76040-038.dmg.trustcache; PREFIX is the directory the file
is written to, keeping the member's basename.
"""
import argparse
from pathlib import Path
import sys
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ipsw', type=Path)
    parser.add_argument('member', help='basename or suffix of the member to extract')
    parser.add_argument('prefix', type=Path, help='directory to write the file into')
    args = parser.parse_args()

    with zipfile.ZipFile(args.ipsw) as archive:
        matches = [name for name in archive.namelist() if name.rsplit('/', 1)[-1] == args.member]
        if not matches:
            print(f'{args.member} not in {args.ipsw.name}', file=sys.stderr)
            for name in archive.namelist():
                if args.member.split('.')[0] in name:
                    print(f'  did you mean {name}', file=sys.stderr)
            return 1
        if len(matches) > 1:
            print(f'{args.member} is ambiguous: {matches}', file=sys.stderr)
            return 1
        target = args.prefix / args.member
        target.parent.mkdir(parents=True, exist_ok=True)
        data = archive.read(matches[0])
        target.write_bytes(data)
        print(f'{matches[0]} -> {target} ({len(data)} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
