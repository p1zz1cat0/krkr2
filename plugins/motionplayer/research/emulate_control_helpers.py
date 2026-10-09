#!/usr/bin/env python3
"""Numerically probe isolated 2016 E-mote control math helpers.

This maps the named PE image at its preferred base and enters only the
constructor/update helpers selected below.  It does not invoke V2Link, TJS,
rendering, or any Windows API.  The script is a research aid: instruction/data
flow remains the authority for field meanings.

Dependencies are intentionally external to the repository:
  PYTHONPATH=/tmp/motionplayer-unicorn python3 ...
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32
from unicorn.x86_const import (
    UC_X86_REG_EAX,
    UC_X86_REG_EBX,
    UC_X86_REG_EBP,
    UC_X86_REG_ECX,
    UC_X86_REG_EDI,
    UC_X86_REG_EIP,
    UC_X86_REG_ESI,
    UC_X86_REG_ESP,
)


PAGE = 0x1000
STACK_BASE = 0x20000000
STACK_SIZE = 0x20000
HEAP_BASE = 0x30000000
HEAP_SIZE = 0x20000
RETURN_SENTINEL = 0x40000000


def align_up(value: int, alignment: int = PAGE) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def pack_f32(value: float) -> bytes:
    return struct.pack("<f", value)


def unpack_f32(value: bytes) -> float:
    return struct.unpack("<f", value)[0]


class ImageHarness:
    def __init__(self, dll: Path) -> None:
        self.pe = pefile.PE(str(dll), fast_load=True)
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        image_base = self.pe.OPTIONAL_HEADER.ImageBase
        image_size = align_up(self.pe.OPTIONAL_HEADER.SizeOfImage)
        self.uc.mem_map(image_base, image_size)
        headers = dll.read_bytes()[: self.pe.OPTIONAL_HEADER.SizeOfHeaders]
        self.uc.mem_write(image_base, headers)
        for section in self.pe.sections:
            raw = section.get_data()
            if raw:
                self.uc.mem_write(image_base + section.VirtualAddress, raw)
        self.uc.mem_map(STACK_BASE, STACK_SIZE)
        self.uc.mem_map(HEAP_BASE, HEAP_SIZE)
        self.uc.mem_map(RETURN_SENTINEL, PAGE)
        self.uc.hook_add(UC_HOOK_CODE, self._stop_at_sentinel)

    def _stop_at_sentinel(self, uc: Uc, address: int, _size: int, _data: object) -> None:
        if address == RETURN_SENTINEL:
            uc.emu_stop()

    def write_f32(self, address: int, value: float) -> None:
        self.uc.mem_write(address, pack_f32(value))

    def read_f32(self, address: int) -> float:
        return unpack_f32(bytes(self.uc.mem_read(address, 4)))

    def call(
        self,
        address: int,
        args: list[float | int],
        *,
        float_args: set[int] | None = None,
        registers: dict[int, int] | None = None,
    ) -> None:
        float_args = float_args or set()
        stack = STACK_BASE + STACK_SIZE // 2
        encoded: list[bytes] = []
        for index, arg in enumerate(args):
            encoded.append(pack_f32(float(arg)) if index in float_args else struct.pack("<I", int(arg)))
        frame = struct.pack("<I", RETURN_SENTINEL) + b"".join(encoded)
        self.uc.mem_write(stack, frame)
        self.uc.reg_write(UC_X86_REG_ESP, stack)
        self.uc.reg_write(UC_X86_REG_EBP, 0)
        self.uc.reg_write(UC_X86_REG_EAX, 0)
        self.uc.reg_write(UC_X86_REG_EBX, 0)
        self.uc.reg_write(UC_X86_REG_ECX, 0)
        self.uc.reg_write(UC_X86_REG_EDI, 0)
        self.uc.reg_write(UC_X86_REG_ESI, 0)
        for register, value in (registers or {}).items():
            self.uc.reg_write(register, value)
        self.uc.reg_write(UC_X86_REG_EIP, address)
        self.uc.emu_start(address, RETURN_SENTINEL, count=2_000_000)


def probe_bust(
    harness: ImageHarness, frames: int, legacy_y_bias: float
) -> dict[str, object]:
    obj = HEAP_BASE + 0x1000
    config = HEAP_BASE + 0x2000
    out_x = HEAP_BASE + 0x3000
    out_y = HEAP_BASE + 0x3004

    # HeapAlloc is not zeroing in the 2016 DLL. Poison the object before the
    # constructor so omitted writes remain visible instead of being masked by
    # Unicorn's freshly mapped zero pages.
    harness.uc.mem_write(obj, b"\xA5" * 0x50)

    # gravity, spring, friction, scale_x, scale_y
    metadata = [0.35, 0.22, 0.08, 1.1, 0.9]
    for index, value in enumerate(metadata):
        harness.write_f32(config + index * 4, value)
    harness.call(
        0x100C39C0,
        [],
        registers={UC_X86_REG_EAX: obj, UC_X86_REG_ECX: config},
    )
    untouched_y_bias = bytes(harness.uc.mem_read(obj + 0x4C, 4)).hex()
    harness.write_f32(obj + 0x4C, legacy_y_bias)

    snapshots: list[dict[str, object]] = []
    for frame in range(frames):
        # x, y, out_x, out_y, force_x/y, fixed-step dt, scale, angle_radians
        args: list[float | int] = [
            12.0,
            -4.0,
            out_x,
            out_y,
            0.6,
            -0.25,
            1.0,
            1.25,
            0.2,
        ]
        harness.call(
            0x100C3CE0,
            args,
            float_args={0, 1, 4, 5, 6, 7, 8},
            registers={UC_X86_REG_ESI: obj},
        )
        snapshots.append(
            {
                "frame": frame,
                "output": [harness.read_f32(out_x), harness.read_f32(out_y)],
                "position": [harness.read_f32(obj + off) for off in (0x34, 0x38, 0x3C)],
                "velocity": [harness.read_f32(obj + off) for off in (0x40, 0x44, 0x48)],
            }
        )
    return {
        "constructor_untouched_0x4c_hex": untouched_y_bias,
        "injected_legacy_y_bias": legacy_y_bias,
        "frames": snapshots,
    }


def probe_pend(
    harness: ImageHarness, frames: int, vertical_reference: float
) -> dict[str, object]:
    obj = HEAP_BASE + 0x4000
    config = HEAP_BASE + 0x5000
    outputs = [HEAP_BASE + 0x6000 + index * 4 for index in range(3)]

    harness.uc.mem_write(obj, b"\xA5" * 0xB0)

    # gravity, friction_x/y, b_rate, v_bound, ud_eft,
    # length[2], scale_x[2], scale_y[2], bend_spd, bend_vol
    metadata = [
        0.35,
        0.08,
        0.11,
        0.25,
        1.4,
        1.0,
        20.0,
        14.0,
        1.1,
        0.8,
        0.9,
        0.7,
        0.16,
        0.42,
    ]
    for index, value in enumerate(metadata):
        if index == 5:
            harness.uc.mem_write(config + index * 4, struct.pack("<I", int(value)))
        else:
            harness.write_f32(config + index * 4, value)
    harness.call(
        0x100C8050,
        [],
        registers={UC_X86_REG_EAX: obj, UC_X86_REG_ECX: config},
    )
    untouched_vertical_reference = bytes(harness.uc.mem_read(obj + 0xA0, 4)).hex()
    harness.write_f32(obj + 0xA0, vertical_reference)

    snapshots: list[dict[str, object]] = []
    for frame in range(frames):
        # x, y, out0/1/2, force_x/y, fixed-step dt, scale, angle_radians
        args: list[float | int] = [
            12.0,
            -4.0,
            *outputs,
            0.6,
            -0.25,
            1.0,
            1.25,
            0.2,
        ]
        harness.call(
            0x100C86C0,
            args,
            float_args={0, 1, 5, 6, 7, 8, 9},
            registers={UC_X86_REG_EBX: obj},
        )
        snapshots.append(
            {
                "frame": frame,
                "output": [harness.read_f32(address) for address in outputs],
                "points": [
                    [harness.read_f32(obj + base + axis * 4) for axis in range(3)]
                    for base in (0x70, 0x7C)
                ],
                "velocity": [
                    [harness.read_f32(obj + base + axis * 4) for axis in range(3)]
                    for base in (0x88, 0x94)
                ],
                "bend": [harness.read_f32(obj + off) for off in (0xA4, 0xA8)],
            }
        )
    return {
        "constructor_untouched_0xa0_hex": untouched_vertical_reference,
        "injected_vertical_reference": vertical_reference,
        "frames": snapshots,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("dll", type=Path)
    parser.add_argument("controller", choices=("bust", "pend"))
    parser.add_argument("--frames", type=int, default=4)
    parser.add_argument(
        "--legacy-y-bias",
        type=float,
        default=0.0,
        help="explicit value injected into EPBustControl +0x4c after construction",
    )
    parser.add_argument(
        "--vertical-reference",
        type=float,
        default=0.0,
        help="explicit value injected into EPPendControl +0xa0 after construction",
    )
    args = parser.parse_args()

    harness = ImageHarness(args.dll)
    result = (
        probe_bust(harness, args.frames, args.legacy_y_bias)
        if args.controller == "bust"
        else probe_pend(harness, args.frames, args.vertical_reference)
    )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
