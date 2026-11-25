#!/usr/bin/env python3
"""Minimal ACPI NHLT parser (Intel Non-HD-Audio Link Table).
Usage: python3 -I nhlt_parse.py nhlt.dat [--hex]"""
import struct
import sys

LINK = {0: "HDA", 1: "DSP", 2: "PDM(DMIC)", 3: "SSP(I2S)", 5: "SoundWire", 6: "UAOL"}
DEV = {0: "BT sideband", 1: "DMIC", 4: "I2S/SSP codec", 0xB: "SSP analog"}
DIR = {0: "render", 1: "capture", 2: "render+capture", 3: "render w/ loopback"}
ARRAY = {0xA: "small linear 2-mic", 0xB: "big linear 2-mic", 0xC: "linear 4-mic 1st geom",
         0xD: "planar L 4-mic", 0xE: "linear 4-mic 2nd geom", 0xF: "vendor defined"}


def hexdump(b, indent="      "):
    for i in range(0, len(b), 16):
        print(indent + " ".join(f"{x:02x}" for x in b[i:i + 16]))


def main():
    d = open(sys.argv[1], "rb").read()
    show_hex = "--hex" in sys.argv
    sig, length = d[:4], struct.unpack_from("<I", d, 4)[0]
    print(f"table {sig.decode()} length {length} (file {len(d)})")
    off = 36
    count = d[off]; off += 1
    print(f"endpoints: {count}")
    for e in range(count):
        start = off
        elen, link, inst, ven, dev, rev, subsys, dtype, direction, vbus = struct.unpack_from("<IBBHHHIBBB", d, off)
        off += 4 + 1 + 1 + 2 + 2 + 2 + 4 + 1 + 1 + 1
        print(f"\n[endpoint {e}] len={elen} link={LINK.get(link, link)} instance={inst} "
              f"vendor=0x{ven:04x} device=0x{dev:04x} rev=0x{rev:x} subsys=0x{subsys:08x} "
              f"type={DEV.get(dtype, dtype)} dir={DIR.get(direction, direction)} vbus={vbus}")
        cfgsize = struct.unpack_from("<I", d, off)[0]; off += 4
        cfg = d[off:off + cfgsize]; off += cfgsize
        print(f"  device specific config: {cfgsize} bytes")
        if link == 2 and cfgsize >= 3:
            vslot, ctype = cfg[0], cfg[1]
            print(f"    virtual_slot={vslot} config_type={'mic array' if ctype == 1 else ctype}", end="")
            if ctype == 1 and cfgsize >= 3:
                at = cfg[2]
                print(f" array_type=0x{at:02x} ({ARRAY.get(at & 0xF, '?')})", end="")
            print()
        if show_hex and cfg:
            hexdump(cfg)
        nfmt = d[off]; off += 1
        print(f"  formats: {nfmt}")
        for f in range(nfmt):
            (tag, ch, rate, avg, align, bits, cbsize, valid, chmask) = struct.unpack_from("<HHIIHHHHI", d, off)
            off += 40  # WAVEFORMATEXTENSIBLE incl. 16-byte SubFormat GUID
            fcsize = struct.unpack_from("<I", d, off)[0]; off += 4
            fcfg = d[off:off + fcsize]; off += fcsize
            print(f"   [{f}] {ch}ch {rate}Hz container={bits}bit valid={valid}bit chmask=0x{chmask:x} "
                  f"-> format blob {fcsize} bytes")
            if show_hex and fcfg:
                hexdump(fcfg[:96])
                if fcsize > 96:
                    print(f"      ... ({fcsize - 96} more bytes)")
        if off - start != elen:
            print(f"  (note: parsed {off - start} bytes, descriptor says {elen}; skipping to declared end)")
            off = start + elen
    if off < len(d):
        print(f"\ntrailing data after endpoints: {len(d) - off} bytes")


if __name__ == "__main__":
    main()
