#!/usr/bin/env python3
"""Convert current Vice City VR saves between Win64 and Quest ARM64 layouts.

The game keeps the original fixed 201,828-byte save container on both
platforms.  The only ABI-dependent block in the current Win64/Quest builds is
CGarages: MSVC stores CStoredCar in 40 bytes, while Android Clang stores it in
36 bytes.  This tool reshapes that block and updates the additive checksum.

It intentionally rejects files that do not match the current save signature;
old retail/32-bit saves can contain additional ABI differences and must not be
silently converted as if they came from the current PC VR build.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


SAVE_SIZE = 201_828
GARAGE_PAYLOAD_SIZE = 7_876
FIXED_GARAGE_PREFIX = 44
STORED_CAR_COUNT = 48
GARAGE_COUNT = 32
GARAGE_SIZE = 184
GARAGE_STORED_CAR_OFFSET = 144

# The current Win64 build writes these fixed-size inner blocks.  Older x86
# saves use different ABI-dependent sizes while retaining the same 201,828-byte
# container and a valid checksum, so size/checksum checks alone are not enough
# to make an import safe.
CURRENT_WIN64_INNER_BLOCKS = (
    (7, "Cranes", 1_160),
    (8, "Pickups", 21_588),
    (9, "Phone", 4_408),
    (18, "PlayerInfo", 416),
)


class SaveConversionError(RuntimeError):
    pass


def align4(value: int) -> int:
    return (value + 3) & ~3


def outer_blocks(data: bytes) -> list[tuple[int, int, int]]:
    """Return (header offset, declared size, payload offset) save blocks."""
    result: list[tuple[int, int, int]] = []
    offset = 0
    checksum_offset = len(data) - 4
    while offset + 4 <= checksum_offset and len(result) < 40:
        declared_size = struct.unpack_from("<I", data, offset)[0]
        payload_offset = offset + 4
        end = payload_offset + align4(declared_size)
        if end > checksum_offset:
            raise SaveConversionError(
                f"block {len(result)} overruns save: offset={offset}, "
                f"size={declared_size}"
            )
        result.append((offset, declared_size, payload_offset))
        offset = end
        if offset == checksum_offset:
            break
    if offset != checksum_offset:
        raise SaveConversionError(
            f"save block stream ends at {offset}, checksum starts at "
            f"{checksum_offset}"
        )
    return result


def find_garage_payload(data: bytes) -> tuple[int, int]:
    matches: list[tuple[int, int]] = []
    for _header, outer_size, payload_offset in outer_blocks(data):
        if outer_size < 4:
            continue
        inner_size = struct.unpack_from("<I", data, payload_offset)[0]
        if inner_size == GARAGE_PAYLOAD_SIZE:
            matches.append((payload_offset + 4, inner_size))
    if len(matches) != 1:
        raise SaveConversionError(
            f"expected exactly one {GARAGE_PAYLOAD_SIZE}-byte garages block, "
            f"found {len(matches)}"
        )
    return matches[0]


def validate_current_win64_layout(data: bytes) -> None:
    """Reject retail/legacy x86 saves before attempting a Quest import."""
    blocks = outer_blocks(data)
    mismatches: list[str] = []
    for block_index, block_name, expected_inner_size in CURRENT_WIN64_INNER_BLOCKS:
        if block_index >= len(blocks):
            mismatches.append(
                f"{block_name} block {block_index}: missing "
                f"(expected inner size {expected_inner_size})"
            )
            continue
        _header, declared_size, payload_offset = blocks[block_index]
        if declared_size < 4:
            observed = f"outer size {declared_size}"
        else:
            observed = str(struct.unpack_from("<I", data, payload_offset)[0])
        if observed != str(expected_inner_size):
            mismatches.append(
                f"{block_name} block {block_index}: expected inner size "
                f"{expected_inner_size}, got {observed}"
            )
    if mismatches:
        raise SaveConversionError(
            "not a current Win64 Vice City VR save; original retail and "
            "legacy x86 saves cannot be imported safely (" + "; ".join(mismatches) + ")"
        )


def pc_car_to_quest(car: bytes) -> bytes:
    if len(car) != 40:
        raise SaveConversionError("invalid Win64 CStoredCar size")
    # Common data through the bitfield's first byte, then six int8 fields.
    return car[:29] + car[32:38] + b"\0"


def quest_car_to_pc(car: bytes) -> bytes:
    if len(car) != 36:
        raise SaveConversionError("invalid Quest CStoredCar size")
    return car[:29] + b"\0\0\0" + car[29:35] + b"\0\0"


def pc_payload_to_quest(payload: bytes) -> bytes:
    if len(payload) != GARAGE_PAYLOAD_SIZE:
        raise SaveConversionError("invalid garages payload size")
    cursor = FIXED_GARAGE_PREFIX
    output = bytearray(payload[:cursor])
    for _ in range(STORED_CAR_COUNT):
        output += pc_car_to_quest(payload[cursor : cursor + 40])
        cursor += 40
    for _ in range(GARAGE_COUNT):
        garage = payload[cursor : cursor + GARAGE_SIZE]
        if len(garage) != GARAGE_SIZE:
            raise SaveConversionError("truncated Win64 CGarage array")
        converted = bytearray(GARAGE_SIZE)
        converted[:GARAGE_STORED_CAR_OFFSET] = garage[:GARAGE_STORED_CAR_OFFSET]
        converted[
            GARAGE_STORED_CAR_OFFSET : GARAGE_STORED_CAR_OFFSET + 36
        ] = pc_car_to_quest(garage[GARAGE_STORED_CAR_OFFSET : GARAGE_SIZE])
        output += converted
        cursor += GARAGE_SIZE
    if cursor + 24 != GARAGE_PAYLOAD_SIZE:
        raise SaveConversionError("unexpected Win64 garages layout")
    output += bytes(GARAGE_PAYLOAD_SIZE - len(output))
    return bytes(output)


def quest_payload_to_pc(payload: bytes) -> bytes:
    if len(payload) != GARAGE_PAYLOAD_SIZE:
        raise SaveConversionError("invalid garages payload size")
    cursor = FIXED_GARAGE_PREFIX
    output = bytearray(payload[:cursor])
    for _ in range(STORED_CAR_COUNT):
        output += quest_car_to_pc(payload[cursor : cursor + 36])
        cursor += 36
    for _ in range(GARAGE_COUNT):
        garage = payload[cursor : cursor + GARAGE_SIZE]
        if len(garage) != GARAGE_SIZE:
            raise SaveConversionError("truncated Quest CGarage array")
        converted = bytearray(GARAGE_SIZE)
        converted[:GARAGE_STORED_CAR_OFFSET] = garage[:GARAGE_STORED_CAR_OFFSET]
        converted[GARAGE_STORED_CAR_OFFSET:GARAGE_SIZE] = quest_car_to_pc(
            garage[
                GARAGE_STORED_CAR_OFFSET : GARAGE_STORED_CAR_OFFSET + 36
            ]
        )
        output += converted
        cursor += GARAGE_SIZE
    if cursor + 216 != GARAGE_PAYLOAD_SIZE:
        raise SaveConversionError("unexpected Quest garages layout")
    output += bytes(GARAGE_PAYLOAD_SIZE - len(output))
    return bytes(output)


def validate_checksum(data: bytes) -> None:
    expected = struct.unpack_from("<I", data, len(data) - 4)[0]
    actual = sum(data[:-4]) & 0xFFFFFFFF
    if expected != actual:
        raise SaveConversionError(
            f"checksum mismatch: file={expected:#010x}, calculated={actual:#010x}"
        )


def convert(data: bytes, direction: str) -> bytes:
    if len(data) != SAVE_SIZE:
        raise SaveConversionError(
            f"unsupported save size {len(data)}; expected {SAVE_SIZE}"
        )
    validate_checksum(data)
    if direction == "pc-to-quest":
        validate_current_win64_layout(data)
    payload_offset, payload_size = find_garage_payload(data)
    payload = data[payload_offset : payload_offset + payload_size]
    if direction == "pc-to-quest":
        converted_payload = pc_payload_to_quest(payload)
    else:
        converted_payload = quest_payload_to_pc(payload)
    result = bytearray(data)
    result[payload_offset : payload_offset + payload_size] = converted_payload
    struct.pack_into("<I", result, len(result) - 4, sum(result[:-4]) & 0xFFFFFFFF)
    validate_checksum(result)
    return bytes(result)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("direction", choices=("pc-to-quest", "quest-to-pc"))
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        source_data = args.source.read_bytes()
        converted = convert(source_data, args.direction)
        args.destination.parent.mkdir(parents=True, exist_ok=True)
        args.destination.write_bytes(converted)
    except (OSError, SaveConversionError) as exc:
        print(f"save conversion failed: {exc}", file=sys.stderr)
        return 1
    print(
        f"converted {args.source} -> {args.destination} "
        f"({args.direction}, {len(converted)} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
