#!/usr/bin/env python3
"""Prove firmware is running, over CMSIS-DAP, without looking at the board.

  verify.py <pyocd-target> <app.elf> [--watch SYMBOL] [--seconds N]

Checks, in order:
  1. every loadable ELF segment vs memory  -> did programming actually land?
  2. VTOR + a vector vs the ELF symbols    -> is the running image the one you built?
  3. SysTick CTRL/LOAD + PC                -> is the core alive and ticking?
  4. polls an SRAM symbol while running    -> is it doing work?
"""
import argparse, subprocess, sys, time
from elftools.elf.elffile import ELFFile
from pyocd.core.helpers import ConnectHelper

def symbols(elf):
    out = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True).stdout
    d = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            d[p[2]] = int(p[0], 16)
    return d

ap = argparse.ArgumentParser()
ap.add_argument("target"); ap.add_argument("elf")
ap.add_argument("--watch", default="g_systickCounter")
ap.add_argument("--seconds", type=float, default=6.0)
a = ap.parse_args()

sym = symbols(a.elf)

with ConnectHelper.session_with_chosen_probe(
        target_override=a.target, connect_mode="halt",
        options={"resume_on_disconnect": True}) as s:
    t = s.target
    for _ in range(50):
        if str(t.get_state()) == "State.HALTED":
            break
        t.halt(); time.sleep(0.02)

    # Compare loadable segments only. A flat .bin would false-positive: objcopy pads
    # inter-section gaps with 0x00 while erased flash reads 0xFF.
    bad = []
    with open(a.elf, "rb") as f:
        for seg in ELFFile(f).iter_segments():
            if seg["p_type"] != "PT_LOAD" or seg["p_filesz"] == 0:
                continue
            addr, data = seg["p_paddr"], seg.data()[: seg["p_filesz"]]
            onchip = bytes(t.read_memory_block8(addr, len(data)))
            off = next((i for i, (x, y) in enumerate(zip(onchip, data)) if x != y), None)
            if off is not None:
                bad.append("0x%08x (+0x%x)" % (addr, off))
    print("1. loadable segments vs memory: %s"
          % ("all identical" if not bad
             else "MISMATCH at " + ", ".join(bad) + " -> programming did not land"))

    vtor = t.read32(0xE000ED08)
    handler = t.read32(vtor + 0x3C) & ~1        # SysTick vector
    want = sym.get("SysTick_Handler")
    print("2. VTOR=0x%08x SysTick vector=0x%08x elf=%s -> %s"
          % (vtor, handler, ("0x%08x" % want) if want else "n/a",
             "match" if want and handler == want else "STALE IMAGE"))

    print("3. PC=0x%08x  SysTick CTRL=0x%08x LOAD=%d"
          % (t.read_core_register("pc"), t.read32(0xE000E010), t.read32(0xE000E014) + 1))

    addr = sym.get(a.watch)
    if addr is None:
        print("4. symbol %r not in elf; skipping" % a.watch); sys.exit(0)
    t.resume()
    seen, t0 = set(), time.time()
    while time.time() - t0 < a.seconds:
        seen.add(t.read32(addr)); time.sleep(0.05)
    print("4. %s took %d distinct values in %.0fs -> %s"
          % (a.watch, len(seen), a.seconds,
             "RUNNING" if len(seen) > 3 else "STUCK (core not progressing)"))
