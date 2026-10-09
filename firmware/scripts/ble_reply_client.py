"""Candidate39 pure BLE reply codec. Serialize original commands; retry only reply.
Identity is boot-local retained-response correlation, NOT exactly-once execution.
"""
from dataclasses import dataclass
from functools import reduce
from operator import xor
import struct

HEADER = struct.Struct('<2sBBIHH')
MAX_TEXT = 8191

@dataclass(frozen=True)
class Packet:
    kind: str
    generation: int = 0
    total: int = 0
    offset: int = 0
    flag: int = 0
    payload: bytes = b''

class ReplyError(ValueError):
    pass

class StaleReply(ReplyError):
    pass

def classify(value: bytes) -> Packet:
    value = bytes(value)
    invalid = Packet('invalid')
    if value.startswith(b'\0R'):
        if not 12 <= len(value) <= 512:
            return invalid
        _, version, flag, gen, total, off = HEADER.unpack_from(value)
        payload = value[12:]
        if version != 1 or not gen or total > MAX_TEXT:
            return invalid
        if flag == 1:
            if off or payload:
                return invalid
            kind = 'metadata'
        elif flag == 2:
            if off > total or off + len(payload) > total or (not payload and off != total):
                return invalid
            kind = 'page'
        elif flag in (0x81, 0x82, 0x83):
            if payload:
                return invalid
            kind = 'error'
        else:
            return invalid
        return Packet(kind, gen, total, off, flag, payload)
    # Outbound snapshots, NOT inbound counted-command routing. Layouts mirror
    # dura_legacy_serialize_snapshot and DURA_LEGACY_NOTIFICATION_MAX_LEN (12).
    # A count alone also matches ordinary text (e.g. 125-byte JSON starting '{').
    if 6 <= len(value) <= 12 and value[0] + 2 == len(value) and value[1] == 0x1d:
        screen, menu = value[2:4]
        if screen == 0x1e and menu == 0x02:
            expected = 12
        elif screen in (0x50, 0x5a):
            expected = 9
        elif screen == 0 and menu in (0x01, 0x02):
            expected = 6
        elif screen == 0 and menu in (0x2c, 0x38):
            expected = 9
        else:
            return invalid
        if len(value) == expected and reduce(xor, value[1:-1], 0) ^ 0xff == value[-1]:
            return Packet('legacy', payload=value)
        return invalid
    try:
        text = value.decode('utf-8')
    except UnicodeDecodeError:
        return invalid
    if not text or any(ord(c) < 32 and c not in '\r\n\t' for c in text) or '\x7f' in text:
        return invalid
    return Packet('text', payload=value)

def metadata_command() -> bytes:
    return b'reply'

def page_command(generation: int, offset: int) -> bytes:
    if not 0 < generation <= 0xffffffff or not 0 <= offset <= 0xffff:
        raise ValueError('generation/offset outside wire range')
    return f'reply {generation:08x} {offset:04x}'.encode('ascii')

class ReplyAssembler:
    def __init__(self, metadata: Packet):
        if metadata.kind != 'metadata' or not 0 < metadata.generation <= 0xffffffff or not 0 <= metadata.total <= MAX_TEXT:
            raise ValueError('valid metadata required')
        self.generation, self.total = metadata.generation, metadata.total
        self.data = bytearray(self.total)
        self.seen = bytearray(self.total)

    def feed(self, value: bytes) -> bool:
        p = classify(value)
        if p.kind not in ('metadata', 'page', 'error'):
            return False
        if p.generation != self.generation or p.total != self.total or p.flag == 0x81:
            raise StaleReply('retained response changed; do not merge or rerun original command')
        if p.kind == 'error':
            raise ReplyError(f'reply error 0x{p.flag:02x}, offset {p.offset}')
        if p.kind == 'page':
            # Validate all overlaps before mutating, so conflict fails atomically.
            for i, b in enumerate(p.payload, p.offset):
                if self.seen[i] and self.data[i] != b:
                    raise ReplyError('conflicting duplicate page')
            self.data[p.offset:p.offset+len(p.payload)] = p.payload
            self.seen[p.offset:p.offset+len(p.payload)] = b'\1' * len(p.payload)
        return True

    def next_offset(self):
        try:
            return self.seen.index(0)
        except ValueError:
            return None

    def result(self):
        return bytes(self.data) if self.next_offset() is None else None
