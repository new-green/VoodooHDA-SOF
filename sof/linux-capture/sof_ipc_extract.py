#!/usr/bin/env python3
"""Turn a Linux SOF dmesg trace (sof_debug=0xA00 + dynamic debug) into an IPC3 replay list.

Usage: python3 -I sof_ipc_extract.py dmesg-trace.txt out.json
Each entry: {"seq", "time", "cmd" (hex), "glb", "type", "size", "payload" (hex, without the 8-byte
sof_ipc_cmd_hdr), "context" (nearest preceding driver line)}."""
import json
import re
import sys

GLB = {0x1: "REPLY", 0x2: "COMPOUND", 0x3: "TPLG", 0x4: "PM", 0x5: "COMP", 0x6: "STREAM",
       0x7: "FW_READY", 0x8: "DAI", 0x9: "TRACE", 0xA: "GDB", 0xB: "TEST", 0xC: "PROBE", 0xD: "DEBUG"}
TYPE = {
    0x3: {0x001: "COMP_NEW", 0x002: "COMP_FREE", 0x003: "COMP_CONNECT", 0x010: "PIPE_NEW",
          0x011: "PIPE_FREE", 0x012: "PIPE_CONNECT", 0x013: "PIPE_COMPLETE", 0x020: "BUFFER_NEW",
          0x021: "BUFFER_FREE"},
    0x4: {0x001: "CTX_SAVE", 0x002: "CTX_RESTORE", 0x003: "CTX_SIZE", 0x004: "CLK_SET",
          0x005: "CLK_GET", 0x006: "CLK_REQ", 0x007: "CORE_ENABLE", 0x008: "GATE"},
    0x5: {0x001: "SET_VALUE", 0x002: "GET_VALUE", 0x003: "SET_DATA", 0x004: "GET_DATA"},
    0x6: {0x001: "PCM_PARAMS", 0x002: "PCM_PARAMS_REPLY", 0x003: "PCM_FREE", 0x004: "TRIG_START",
          0x005: "TRIG_STOP", 0x006: "TRIG_PAUSE", 0x007: "TRIG_RELEASE", 0x008: "TRIG_DRAIN",
          0x009: "TRIG_XRUN", 0x00A: "POSITION", 0x010: "VORBIS_PARAMS", 0x011: "VORBIS_FREE"},
    0x8: {0x001: "CONFIG", 0x002: "LOOPBACK"},
}

TX = re.compile(r"\[\s*([\d.]+)\].*ipc tx succeeded: 0x([0-9a-f]{8})")
PAY = re.compile(r"Message payload: ([0-9a-f]{8}): ((?:[0-9a-f]{8} ?)+)")
CTX = re.compile(r"sof-audio-pci-intel-tgl [^:]+: (widget .*|setting up route .*|pcm\d+ .*Entry: \w+|tplg: .*)$")


def main():
    lines = open(sys.argv[1], errors="replace").read().splitlines()
    out, cur, ctx = [], None, ""
    for line in lines:
        m = CTX.search(line)
        if m:
            ctx = m.group(1)
        m = TX.search(line)
        if m:
            if cur:
                out.append(cur)
            cmd = int(m.group(2), 16)
            g, t = cmd >> 28, (cmd >> 16) & 0xFFF
            cur = {"seq": len(out), "time": float(m.group(1)), "cmd": f"0x{cmd:08x}",
                   "glb": GLB.get(g, hex(g)), "type": TYPE.get(g, {}).get(t, hex(t)),
                   "payload": "", "context": ctx}
            continue
        m = PAY.search(line)
        if m and cur is not None:
            words = m.group(2).split()
            # dump shows little-endian u32 words; store as raw little-endian bytes
            cur["payload"] += "".join(int(w, 16).to_bytes(4, "little").hex() for w in words)
    if cur:
        out.append(cur)
    for e in out:
        e["size"] = 8 + len(e["payload"]) // 2
    json.dump(out, open(sys.argv[2], "w"), indent=1)
    for e in out:
        print(f"{e['seq']:3d} {e['time']:10.6f} {e['cmd']} {e['glb']:8} {e['type']:14} "
              f"{e['size']:5d}B  {e['context'][:60]}")


if __name__ == "__main__":
    main()
