#!/usr/bin/env python3
"""Validate the C bank decompressor against the reference dumps.

Builds tests/dump_banks.c, runs it to get all 10 decompressed 55,296-byte
matrix images, then parses the per-field comment dumps in the reference
banks.s (e.g. "; Bank 1 - Effect 1: Sub-Effect 1 - Byte 0-4: dc.l $00148000")
and diffs every documented long.

Usage: python3 scripts/validate_banks.py [path-to-vlm-ref]
"""
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VLM_REF = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO.parent / "vlm-ref"
BANKS_S = VLM_REF / "src" / "vlm" / "banks.s"

EFFECT_SIZE = 6144
FXOBJ_SIZE = 1024
MATRIX_SIZE = 9 * EFFECT_SIZE

DUMP_RE = re.compile(
    r";\s*Bank\s+(\d+)\s*-\s*Effect\s+(\d+):\s*Sub-Effect\s+(\d+)\s*-\s*"
    r"Byte\s+(\d+)-\d+:.*?dc\.l\s+\$([0-9A-Fa-f]{1,8})")


def build_and_run() -> bytes:
    exe = REPO / "bin-test" / "dump_banks"
    exe.parent.mkdir(exist_ok=True)
    subprocess.run(
        ["cc", "-std=gnu17", "-O1", "-fsanitize=undefined,address",
         str(REPO / "tests" / "dump_banks.c"),
         str(REPO / "src" / "banks.c"),
         str(REPO / "src" / "banks_data.c"),
         "-o", str(exe)],
        check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True).stdout
    assert len(out) == 10 * MATRIX_SIZE, f"got {len(out)} bytes"
    return out


def parse_av1_blob() -> tuple[bytes, list[int]]:
    src = (REPO / "src" / "banks_data.c").read_text()
    m = re.search(r"vlm_av1\[VLM_AV1_SIZE\] = \{(.*)\};", src, re.S)
    blob = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})",
                                                m.group(1)))
    m = re.search(r"vlm_bank_off\[VLM_NBANKS\] = \{(.*?)\}", src)
    offs = [int(x, 16) for x in re.findall(r"0x([0-9A-F]{4})", m.group(1))]
    return blob, offs


def unpack(blob: bytes, src: int, dst: bytearray) -> int:
    """The gm patch loop (vlm.s:7054). Returns src after the bail pair."""
    pos = 0
    while True:
        skip, value = blob[src], blob[src + 1]
        src += 2
        if pos >= EFFECT_SIZE:
            break
        pos += skip
        if pos >= EFFECT_SIZE:
            break
        dst[pos] = value
        pos += 1
    return src


def continuous_model(blob: bytes, offs: list[int],
                     bank1: bytes) -> bytes:
    """Reproduce how the reference dumps were generated: one continuous
    pass over all banks, each bank's effect 1 layered on the previous
    bank's effect 9 with the usual rewind. This does NOT match runtime
    (gm resets to refblock per bank) — see docs/analysis-banks-data.md.
    Returns the 10-bank concatenation; bank 1 is taken as given."""
    out = bytearray(bank1)
    src = offs[0]
    scratch = bytearray(bank1[:EFFECT_SIZE])
    src = unpack(blob, src, scratch)
    for _ in range(1, 9):
        src = unpack(blob, src - 2, scratch)
    prev = bytearray(out[8 * EFFECT_SIZE:9 * EFFECT_SIZE])
    for bank in range(1, 10):
        assert src - 2 == offs[bank] or src == offs[bank], \
            f"stream discontinuity at bank {bank + 1}: {src:#x} vs " \
            f"{offs[bank]:#x}"
        matrix = bytearray()
        cur = bytearray(prev)
        src = unpack(blob, src - 2, cur)
        matrix += cur
        for _ in range(1, 9):
            cur = bytearray(cur)
            src = unpack(blob, src - 2, cur)
            matrix += cur
        prev = matrix[8 * EFFECT_SIZE:9 * EFFECT_SIZE]
        out += matrix
    return bytes(out)


def check_against_abs(blob: bytes) -> bool:
    """Byte-compare the extracted blob against the original vlm.abs
    binary (the authoritative check)."""
    abs_path = VLM_REF / "src" / "unused" / "objs" / "vlm.abs"
    if not abs_path.exists():
        print("note: vlm.abs not found, skipping binary comparison")
        return True
    abs_data = abs_path.read_bytes()
    idx = abs_data.find(blob[:16])
    if idx < 0 or abs_data[idx:idx + len(blob)] != blob:
        print("FAIL: extracted av1 blob differs from vlm.abs")
        return False
    print(f"OK: av1 blob byte-identical to vlm.abs (offset {idx:#x})")
    return True


def main() -> None:
    data = build_and_run()
    blob, offs = parse_av1_blob()
    abs_ok = check_against_abs(blob)

    # The banks.s comment dumps were generated continuously across
    # banks; runtime (gm) resets to refblock per bank. Bank 1 is
    # identical under both models and validates the C decompressor
    # (refblock defaults + patch loop). Banks 2+ validate the stream
    # parsing via the continuous model.
    continuous = continuous_model(blob, offs, data[:MATRIX_SIZE])

    # Banks 6-10 comment dumps come from an older data revision and
    # match neither model (see docs/analysis-banks-data.md §4.5); only
    # banks 1-5 gate the result.
    checked = mismatched = stale = 0
    for m in DUMP_RE.finditer(BANKS_S.read_text()):
        bank, effect, sub, byte_off = (int(m.group(i)) for i in range(1, 5))
        expect = int(m.group(5), 16)
        pos = ((bank - 1) * MATRIX_SIZE + (effect - 1) * EFFECT_SIZE +
               (sub - 1) * FXOBJ_SIZE + byte_off)
        got = int.from_bytes(continuous[pos:pos + 4], "big")
        if bank > 5:
            stale += got != expect
            continue
        checked += 1
        if got != expect:
            mismatched += 1
            if mismatched <= 20:
                print(f"MISMATCH bank {bank} effect {effect} sub {sub} "
                      f"byte {byte_off}: expected ${expect:08X}, "
                      f"got ${got:08X}")
    print(f"banks 1-5: checked {checked} documented longs, "
          f"{mismatched} mismatches (continuous model)")
    print(f"banks 6-10: {stale} longs differ (stale reference dumps, "
          f"informational only)")
    sys.exit(1 if mismatched or not checked or not abs_ok else 0)


if __name__ == "__main__":
    main()
