#!/usr/bin/env python3
"""patch_legacysar_kernel.py -- hexpatch skip_initramfs -> want_initramfs in an Image.gz-dtb.

This device is a legacy system-as-root (SAR) device (research-notes/21): the stock kernel has
skip_initramfs compiled in, so it ALWAYS bypasses the boot.img ramdisk and tries to mount
system-as-root directly -- regardless of what's in the ramdisk. Magisk's own boot_patch.sh already
does exactly this hexpatch when invoked with LEGACYSAR=true, and research-notes/21 proved it's the
actual, sufficient fix (byte-identical to the known-working Magisk kernel once applied).
components/kernel's own build output has never carried this patch (its own "builds and boots" claim
paired with a Magisk-repacked ramdisk going through the SAME magiskboot LEGACYSAR path separately --
this script is what makes components/os's own from-scratch boot.img need no external tool for it).

Usage: patch_legacysar_kernel.py <in Image.gz-dtb> <out Image.gz-dtb>
"""
import sys
import zlib

OLD = b"skip_initramfs\x00"
NEW = b"want_initramfs\x00"
assert len(OLD) == len(NEW)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]

    with open(src, 'rb') as f:
        data = f.read()

    # The kernel portion is a single gzip member; whatever's left after it decompresses is the
    # raw, uncompressed DTB blob appended after -- preserved byte for byte, untouched.
    d = zlib.decompressobj(zlib.MAX_WBITS | 16)
    kernel = d.decompress(data) + d.flush()
    dtb_tail = d.unused_data

    count = kernel.count(OLD)
    if count != 1:
        raise SystemExit(f"expected exactly one occurrence of {OLD!r}, found {count} -- "
                          f"refusing to patch blindly (kernel build may have changed)")
    kernel = kernel.replace(OLD, NEW)

    comp = zlib.compressobj(9, zlib.DEFLATED, zlib.MAX_WBITS | 16)
    kernel_gz = comp.compress(kernel) + comp.flush()

    with open(dst, 'wb') as f:
        f.write(kernel_gz + dtb_tail)
    print(f"patched {src} -> {dst} ({len(data)} -> {len(kernel_gz) + len(dtb_tail)} bytes)")


if __name__ == '__main__':
    main()
