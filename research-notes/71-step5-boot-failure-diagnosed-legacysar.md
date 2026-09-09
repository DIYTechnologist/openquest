# Step 5: boot failure diagnosed — the same LEGACYSAR issue from research-notes/21, never applied to this build — 2026-09-09

Continuation of `research-notes/70` on the same day. Before guessing at the early-boot failure,
checked this project's own history first — and it had already been solved once, for a different
reason, months ago.

## The match

`research-notes/21` (kernel bring-up, 2026-09-03) hit an identical symptom flashing an
instrumented-but-otherwise-normal boot image: device cycled without booting, recovered cleanly from
backup. Root cause, found there: **this is a legacy system-as-root (SAR) device.** The stock kernel
is built with `skip_initramfs` — meaning it *always* bypasses whatever ramdisk is in `boot.img` and
tries to mount system-as-root directly, regardless of the ramdisk's contents. Magisk's own
`boot_patch.sh` (`LEGACYSAR=true`) works around this by hexpatching the literal string
`skip_initramfs\0` to `want_initramfs\0` **inside the kernel binary itself** — same length, direct
byte substitution. Without that patch, any ramdisk-dependent boot flow is a guaranteed early
failure, which is exactly what step 5's from-scratch AOSP ramdisk needed and never got.

`components/kernel`'s own build has never carried this patch — it "builds and boots" today only
because Magisk applies the identical hexpatch separately, through `magiskboot`, when repacking the
*stock* ramdisk. Nothing in the new AOSP/LineageOS build path (`components/os`) knew this device
needed it at all, since `BoardConfig.mk` was written by adapting phone reference trees that have no
such quirk.

## Verified directly, not assumed

```
gunzip -c components/kernel/build/out/arch/arm64/boot/Image.gz-dtb | strings | grep skip_initramfs
-> 1 occurrence, 0 want_initramfs   -- confirms the hypothesis before touching anything
```

## Fix: a permanent build step, not a one-off patch

New `components/os/patch_legacysar_kernel.py`: splits `Image.gz-dtb` into its gzip-compressed
kernel member and the raw, uncompressed DTB blob appended after it (tracked via
`zlib.decompressobj`'s `unused_data`, not guessed at an offset), hexpatches the kernel, recompresses,
reassembles. Wired into `components/os/Makefile`'s `kernel-prebuilt` target so every future build
gets it automatically — this isn't a manual step to remember, it happens every time
`components/kernel`'s output is consumed by `components/os`.

Verified the patch survived into the actual rebuilt `boot.img` (not just the intermediate prebuilt
file): unpacked the real boot image's embedded kernel and confirmed `want_initramfs` present,
`skip_initramfs` gone. Rebuild itself took 11 seconds — ninja correctly recognised only the kernel
prebuilt had changed and rebuilt just `boot.img`/`system.img`'s install step, not the whole tree.

## What's next

Flash and test. If this is the actual root cause (strong prior: identical symptom, identical device,
already-proven mechanism), this should be the fix that gets past the early-boot failure. If it
isn't, or only gets partially further, that's real information too — worth stating as a real test,
not a foregone conclusion.
