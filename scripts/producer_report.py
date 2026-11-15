#!/usr/bin/env python3
"""Producer-side self-check for the new architecture (fakerFG + xess_mfg_producer).

Reads the LAST session of `<exe>_mfg_producer.log` and `<exe>_fakerFG.log` and answers one
question: did the transport do what it is supposed to do? No bridge needed for most of it.

Usage:
    producer_report.py [<game exe dir>]          # default: the CP2077 Steam install
    producer_report.py --log <producer.log> --fg-log <fakerFG.log>

Checks (PASS/FAIL):
  1. xess_mfg_producer.dll loaded and subscribed to the fakerFG record (API v2)
  2. shared block created (slots, format, content size)
  3. fakerFG published a record (link mode: "record: upscaler lane ...")
  4. MV copied (size/format/declared state/list type) -- depth is reported separately
  5. at least one fence was signalled ("publish #")
  6. no hard errors (Signal FAILED / no device / CreatePlacedResource FAILED / CreateFence)
Informational:
  * credit window (ON only when a consumer is ACKing -> bridge running)
  * XeFG still generating (2X present status fgEnabled=1) in the fakerFG log
  * measured bytes per frame at 1:1 (MV + depth, 4 bytes/px)
"""
import os
import re
import sys

DEFAULT_DIR = "/mnt/e/SteamLibrary/steamapps/common/Cyberpunk 2077/bin/x64"


def last_session(text, marker_re):
    lines = text.splitlines()
    hits = [i for i, ln in enumerate(lines) if re.search(marker_re, ln)]
    return "\n".join(lines[hits[-1]:]) if hits else text


def find(directory, suffix):
    if not os.path.isdir(directory):
        return None
    suffix = suffix.lower()
    for name in sorted(os.listdir(directory)):
        if name.lower().endswith(suffix) and os.path.isfile(os.path.join(directory, name)):
            return os.path.join(directory, name)
    return None


def main(argv):
    producer_path = fg_path = None
    if "--log" in argv:
        producer_path = argv[argv.index("--log") + 1]
        if "--fg-log" in argv:
            fg_path = argv[argv.index("--fg-log") + 1]
    else:
        directory = argv[1] if len(argv) > 1 else DEFAULT_DIR
        producer_path = find(directory, "_mfg_producer.log")
        fg_path = find(directory, "_fakerFG.log")

    if producer_path is None or not os.path.isfile(producer_path):
        print("no <exe>_mfg_producer.log found - is xess_mfg_producer.dll next to the exe?")
        return 2

    ptext = last_session(open(producer_path, encoding="utf-8", errors="replace").read(),
                         r"==== xess_mfg_producer pid=")
    ftext = ""
    if fg_path and os.path.isfile(fg_path):
        ftext = last_session(open(fg_path, encoding="utf-8", errors="replace").read(),
                             r"DllMain: attached as dxgi\.dll")

    def has(text, pat):
        return re.search(pat, text) is not None

    subscribed = has(ptext, r"producer: subscribed to the fakerFG record")
    shared = re.search(r"shared ready: .*?used=([0-9.]+) MB slots=(\d+) mv (\d+)x(\d+) fmt (\d+)"
                       r" depth (\d+)x(\d+) fmt (\d+)", ptext)
    record = has(ftext, r"record: upscaler lane")
    mv_copy = re.search(r"\[mv\] copy (\d+)x(\d+) fmt (\d+) \(declared state (0x[0-9A-Fa-f]+), "
                        r"list type (\d+)\)", ptext)
    depth_copy = re.search(r"\[depth\] copy (\d+)x(\d+) fmt (\d+)", ptext)
    publishes = [int(x) for x in re.findall(r"publish #(\d+) ", ptext)]
    credit_on = has(ptext, r"credit window ON")
    credit_off = has(ptext, r"credit window OFF")

    errors = [label for pat, label in [
        (r"Signal FAILED", "fence Signal FAILED"),
        (r"record has no device", "record carried no device"),
        (r"CreatePlacedResource FAILED", "slot placement FAILED"),
        (r"CreateFence FAILED", "fence creation FAILED"),
        (r"CreateFileMapping FAILED|MapViewOfFile FAILED|OpenExistingHeapFromFileMapping FAILED",
         "shared block FAILED"),
        (r"subscribe FAILED", "subscribe FAILED"),
    ] if has(ptext, pat)]

    fg_2x = has(ftext, r"2X present status[^\n]*fgEnabled=1")
    fg_1x = has(ftext, r"1X present status[^\n]*fgEnabled=0")
    fg_chain = has(ftext, r"the swap chain is ours now|the title keeps the library's swap chain")

    def line(ok, text):
        print(f"  [{'PASS' if ok else 'FAIL'}] {text}")

    print(f"producer log: {producer_path}")
    if fg_path:
        print(f"fakerFG log : {fg_path}")
    print()
    ok = True
    line(subscribed, "producer loaded and subscribed to the fakerFG record")
    ok &= subscribed
    line(shared is not None, "shared block created"
         + (f" ({shared.group(1)} MB, {shared.group(2)} slots, mv {shared.group(3)}x{shared.group(4)}"
            f" fmt {shared.group(5)}, depth fmt {shared.group(8)})" if shared else ""))
    ok &= shared is not None
    if ftext:
        line(record, "fakerFG published a frame record (record: upscaler lane)")
        ok &= record
    line(mv_copy is not None, "motion vectors copied"
         + (f" ({mv_copy.group(1)}x{mv_copy.group(2)} fmt {mv_copy.group(3)}, declared state"
            f" {mv_copy.group(4)}, list type {mv_copy.group(5)})" if mv_copy else ""))
    ok &= mv_copy is not None
    line(depth_copy is not None, "depth copied"
         + (f" ({depth_copy.group(1)}x{depth_copy.group(2)} fmt {depth_copy.group(3)})"
            if depth_copy else ""))
    ok &= depth_copy is not None
    line(bool(publishes), f"fence signalled (publish lines: {publishes[-1] if publishes else 0})")
    ok &= bool(publishes)
    line(not errors, "no transport errors" + (f" ({', '.join(errors)})" if errors else ""))
    ok &= not errors

    print()
    if shared:
        w, h = int(shared.group(3)), int(shared.group(4))
        per = w * h * 4 / 1048576.0
        print(f"bytes/frame: {per:.1f} MB mv + {per:.1f} MB depth = {2*per:.1f} MB (1:1, 4 B/px)")
    print("consumer: " + ("ACKing (credit window ON)" if credit_on
                          else ("not ACKing (round-robin)" if credit_off else "no state yet")))
    if not credit_on:
        print("          -> start bridge.exe to consume; the producer works without it")
    if ftext:
        fg_state = ('generating locally (2X, fgEnabled=1)' if fg_2x else
                    ('NOT generating (1X) - expected for consume/bridge' if fg_1x else
                     'no present status yet'))
        print(f"XeFG chain: {fg_state} | chain taken over: {'yes' if fg_chain else 'no'}")
    print()
    print("RESULT:", "PRODUCER OK" if ok else "PRODUCER NOT OK")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

