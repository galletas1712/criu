#!/usr/bin/env python3
"""
Workload that the compression benchmark dumps and restores.

Provides helpers used by both the long-running workload process (this file's
__main__ block) and the benchmark driver (main.py) to fill memory and compute
expected checksums without re-implementing the generators.

Patterns:
  zero    - all pages zero
  random  - pseudorandom (seed = 42), incompressible
  mixed   - 50% zero, 25% repeating, 25% random
  text    - JSON-shaped records with repeated keys
  elf     - concatenated system binaries
"""

import hashlib
import mmap
import os
import random
import signal
import sys

PAGE_SIZE = 4096
CHUNK_SIZE = 1024 * 1024


def _iter_randbytes(rng, size, chunk_size=CHUNK_SIZE):
    remaining = size
    while remaining:
        n = min(remaining, chunk_size)
        yield rng.randbytes(n)
        remaining -= n


def _repeat_to_size(data, size):
    if not data:
        return
    full, tail = divmod(size, len(data))
    for _ in range(full):
        yield data
    if tail:
        yield data[:tail]


def _gen_zero(size):
    return b"".join(_iter_zero(size))


def _gen_random(size):
    return b"".join(_iter_random(size))


def _gen_mixed(size):
    return b"".join(_iter_mixed(size))


def _iter_zero(size):
    for chunk in _repeat_to_size(bytes(CHUNK_SIZE), size):
        yield chunk


def _iter_random(size):
    rng = random.Random(42)
    yield from _iter_randbytes(rng, size)


def _iter_mixed(size):
    rng = random.Random(42)
    half = size // 2
    quarter = size // 4
    yield from _iter_zero(half)                         # zero half
    yield from _repeat_to_size(b"CRIU" * 1024, quarter) # repeating quarter
    yield from _iter_randbytes(rng, size - half - quarter) # random tail


def _gen_text(size):
    """JSON-like records with repeated keys; LZ4 finds long matches once
    the dictionary warms up. Pre-generates a value pool so we stay fast
    at large sizes."""
    rng = random.Random(42)
    keys = [b"id", b"name", b"created_at", b"status", b"score",
            b"description", b"owner", b"tags"]
    pool = [bytes(c % 75 + 48 for c in rng.randbytes(16)) for _ in range(4096)]

    buf = bytearray()
    pi = 0
    while len(buf) < size:
        rec = b"{"
        for k in keys:
            rec += b'"' + k + b'":"' + pool[pi & 4095] + b'",'
            pi += 1
        rec = rec[:-1] + b"}\n"
        buf += rec
    return bytes(buf[:size])


def _iter_text(size):
    rng = random.Random(42)
    keys = [b"id", b"name", b"created_at", b"status", b"score",
            b"description", b"owner", b"tags"]
    pool = [bytes(c % 75 + 48 for c in rng.randbytes(16)) for _ in range(4096)]

    buf = bytearray()
    pi = 0
    while len(buf) < size:
        while len(buf) < CHUNK_SIZE and len(buf) < size:
            rec = b"{"
            for k in keys:
                rec += b'"' + k + b'":"' + pool[pi & 4095] + b'",'
                pi += 1
            rec = rec[:-1] + b"}\n"
            buf += rec
        out = bytes(buf[:min(len(buf), size)])
        yield out
        size -= len(out)
        del buf[:len(out)]


def _gen_elf(size):
    """Concatenated read-only system binaries; mostly incompressible code
    with a few compressible string tables. Falls back to random if no
    binaries are readable."""
    chunks = []
    paths = ["/bin/bash", "/usr/bin/python3", "/bin/cp", "/bin/tar",
             "/usr/bin/grep", "/usr/bin/awk", "/usr/bin/ls"]
    for p in paths:
        try:
            with open(p, "rb") as f:
                chunks.append(f.read())
        except OSError:
            pass  # Binary missing or unreadable; skip it (random fallback below)
    if not chunks:
        return _gen_random(size)
    out = bytearray()
    i = 0
    while len(out) < size:
        out += chunks[i % len(chunks)]
        i += 1
    return bytes(out[:size])


def _iter_elf(size):
    chunks = []
    paths = ["/bin/bash", "/usr/bin/python3", "/bin/cp", "/bin/tar",
             "/usr/bin/grep", "/usr/bin/awk", "/usr/bin/ls"]
    for p in paths:
        try:
            with open(p, "rb") as f:
                chunks.append(f.read())
        except OSError:
            pass  # Binary missing or unreadable; skip it (random fallback below)
    if not chunks:
        yield from _iter_random(size)
        return

    i = 0
    buf = bytearray()
    while size:
        while len(buf) < CHUNK_SIZE and size:
            chunk = chunks[i % len(chunks)]
            take = min(len(chunk), size)
            buf += chunk[:take]
            size -= take
            i += 1
        yield bytes(buf)
        buf.clear()


_GENERATORS = {
    "zero":   _gen_zero,
    "random": _gen_random,
    "mixed":  _gen_mixed,
    "text":   _gen_text,
    "elf":    _gen_elf,
}

_ITERATORS = {
    "zero":   _iter_zero,
    "random": _iter_random,
    "mixed":  _iter_mixed,
    "text":   _iter_text,
    "elf":    _iter_elf,
}


def supported_patterns():
    return list(_GENERATORS)


def fill_pattern(pattern, size):
    """Return `size` bytes of the named pattern. Used both as the source
    of truth in the workload process and by the driver's expected
    checksum routine."""
    if pattern not in _GENERATORS:
        raise ValueError(f"unknown pattern {pattern!r}")
    return _GENERATORS[pattern](size)


def iter_pattern(pattern, size):
    """Yield the named pattern in bounded chunks."""
    if pattern not in _ITERATORS:
        raise ValueError(f"unknown pattern {pattern!r}")
    yield from _ITERATORS[pattern](size)


def pattern_checksum(pattern, size):
    h = hashlib.sha256()
    for chunk in iter_pattern(pattern, size):
        h.update(chunk)
    return h.hexdigest()


def _main():
    # argv: workload.py SIZE PATTERN PID_PATH HASH_PATH
    size = int(sys.argv[1])
    pattern = sys.argv[2]
    pid_path = sys.argv[3]
    hash_path = sys.argv[4]

    m = mmap.mmap(-1, size, mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS,
                  mmap.PROT_READ | mmap.PROT_WRITE)
    offset = 0
    for chunk in iter_pattern(pattern, size):
        m[offset:offset + len(chunk)] = chunk
        offset += len(chunk)

    with open(hash_path, "w") as f:
        f.write(pattern_checksum(pattern, size))
    with open(pid_path, "w") as f:
        f.write(str(os.getpid()))

    signal.sigwait([signal.SIGTERM])


if __name__ == "__main__":
    _main()
