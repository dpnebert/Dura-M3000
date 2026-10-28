#!/usr/bin/env python3
"""Read-only retained-reference evidence; no compiler emulation or firmware edits."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    root = args.reference.resolve()
    names = ('V2_Meter.mbas', 'Configuration.mbas', 'EEPROM.mbas',
             'V2_Meter.lst', 'Configuration.asm', 'V2_Meter.bin', 'V2_Meter.mbpar')
    hashes = {str(root/n): hashlib.sha256((root/n).read_bytes()).hexdigest() for n in names}
    # Original generated listing contains non-UTF8 text. Latin1 preserves byte values.
    listing = (root/'V2_Meter.lst').read_text(encoding='latin1')
    body = listing.split('__FloatToSignedIntegral:\n', 1)[1].split('; end of __FloatToSignedIntegral', 1)[0]
    records = [(int(a, 16), int(b, 16)) for a, b in
               re.findall(r'^0x([0-9A-Fa-f]+)\s+0x([0-9A-Fa-f]{4})\s', body, re.M)]
    assert records and all(b[0] == a[0]+2 for a, b in zip(records, records[1:]))
    code = b''.join(struct.pack('<H', value) for address, value in records)
    binary = (root/'V2_Meter.bin').read_bytes()
    start = records[0][0]
    assert binary[start:start+len(code)] == code, 'Listing helper does not match retained binary'
    (args.out/'FloatToSignedIntegral.lst.txt').write_text('__FloatToSignedIntegral:\n'+body)
    caller = listing.split('_batchStartPressFunc:\n', 1)[1].split('; end of _batchStartPressFunc', 1)[0]
    calls = re.findall(r'^0x([0-9A-Fa-f]+)\s+0x([0-9A-Fa-f]{8})\s+BL\s+__FloatToSignedIntegral\+0', caller, re.M)
    assert len(calls) == 3
    for address, word in calls:
        machine = struct.pack('<HH', int(word[:4], 16), int(word[4:], 16))
        assert binary[int(address, 16):int(address, 16)+4] == machine
    (args.out/'batchStartPressFunc.lst.txt').write_text('_batchStartPressFunc:\n'+caller)
    result = {
        'input_sha256': hashes,
        'helper_start': hex(start), 'helper_bytes': len(code), 'helper_instructions': len(records),
        'helper_sha256': hashlib.sha256(code).hexdigest(),
        'helper_matches_binary_at_listing_offset': True,
        'batch_start_conversion_calls': [hex(int(address, 16)) for address, _ in calls],
        'all_three_call_opcodes_match_binary': True,
        'status': 'Static listing/binary evidence only; original compiler and device not executed',
        'interpretation': 'Helper discards fractional bits with LSRS; no round-half-up addition. Finite in-range positive floats truncate toward zero.',
        'limits': 'Not a complete firmware source-to-image rebuild; does not establish shipped image, FP divide equivalence, or desired new count policy.',
    }
    (args.out/'reference-evidence.json').write_text(json.dumps(result, indent=2)+'\n')
    assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == h for p, h in hashes.items())
    print(json.dumps(result, indent=2))
    print('PASS helper byte match; PASS three caller opcode matches; PASS reference non-drift')


if __name__ == '__main__':
    main()
