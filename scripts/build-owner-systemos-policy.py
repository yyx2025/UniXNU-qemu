#!/usr/bin/env python3
"""Sign and fully verify the original SystemOS cryptex image: build/verified-root-systemos.

The counterpart of UniXNU's build-owner-os-policy.py for Cryptex1,SystemOS, which no UniXNU script covers (UniXNU#7).
build-verified-root-probe.py --include-systemos reads the result. The SystemOS member of the IPSW is an uncompressed
APFS container, so the policy covers the file itself; QEMU serves the same file as the second VirtIO disk. The volume
UUID is the one offline-owner-systemos-mount.patch looks for. The owner key is the OS policy's
(build/verified-root-host/keys), so both policies share one anchor.
"""
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import uuid

ROOT = Path(__file__).resolve().parents[1]
source = ROOT / '.cache/firmware/target-volumes/044-76034-038.dmg'
out = ROOT / 'build/verified-root-systemos'
expected_uuid = uuid.UUID('8846FE78-2910-4297-AF97-D59BB6F8FC16')
expected_sha256 = 'b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f'   # [ipsw.sha256] of board.toml
if not source.is_file():
    raise SystemExit(f'{source} missing (extract-target-volume.py Cryptex1,SystemOS)')
keys = ROOT / 'build/verified-root-host/keys'
os_key = ROOT / 'build/verified-root-os/trusted-key.bin'
if not os_key.is_file():
    raise SystemExit('Build the OS policy first (build-owner-os-policy.py): both policies share its anchor')
spec = importlib.util.spec_from_file_location('verified_manifest', ROOT / 'kits/VerifiedRoot/tools/manifest.py')
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)
size = source.stat().st_size
with source.open('rb', buffering=1024 * 1024) as stream:
    record = manifest.make(stream, size, keys / 'owner.pem', keys / 'owner-public.pem', expected_uuid, 1, out)
if record['image_sha256'] != expected_sha256:
    raise SystemExit('The image is not the original SystemOS member')
if (out / 'trusted-key.bin').read_bytes() != os_key.read_bytes():
    raise SystemExit('The SystemOS policy must use the anchor of the OS policy')


class View(ctypes.Structure):
    _fields_ = [('hashes', ctypes.c_void_p), ('image_bytes', ctypes.c_uint64), ('block_count', ctypes.c_uint64),
                ('generation', ctypes.c_uint64), ('volume_uuid', ctypes.c_uint8 * 16), ('image_sha256', ctypes.c_uint8 * 32)]


native = ctypes.CDLL(str(ROOT / 'build/verified-root-host/verified-root.dylib'))
native.vr_host_open.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(View)]
native.vr_host_block.argtypes = [ctypes.POINTER(View), ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
encoded = ctypes.create_string_buffer((out / 'manifest.bin').read_bytes())
anchor = ctypes.create_string_buffer((out / 'trusted-key.bin').read_bytes())
view = View()
if native.vr_host_open(encoded, len(encoded) - 1, anchor, 1, ctypes.byref(view)) or bytes(view.volume_uuid) != expected_uuid.bytes:
    raise SystemExit('Real C signature/volume authentication failed')
complete = hashlib.sha256()
with source.open('rb', buffering=1024 * 1024) as stream:
    for index in range(view.block_count):
        block = stream.read(min(4096, view.image_bytes - index * 4096))
        if native.vr_host_block(ctypes.byref(view), index, block, len(block)) != 0:
            raise SystemExit('Actual C block verification failed at ' + str(index))
        complete.update(block)
    if stream.read(1):
        raise SystemExit('The image exceeded its authenticated bounds')
if complete.digest() != bytes(view.image_sha256) or complete.hexdigest() != expected_sha256:
    raise SystemExit('Actual complete original SystemOS digest differs')
report = dict(image_bytes=view.image_bytes, blocks_verified=view.block_count, image_sha256=complete.hexdigest(),
              volume_uuid=str(expected_uuid).upper(), generation=view.generation, source_format='raw APFS container',
              trusted_key_sha256=record['trusted_key_sha256'], real_c_signature_and_block_verification=True,
              original_image_modified=False, host_only=True)
(out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: original SystemOS signature and every', view.block_count, 'block verified by real C/RSA')
