# Step 5 build: eight real bugs from first draft to 92% — config, OOM, silent LFS pointers, hex partition sizes — 2026-09-09

Continuation of `research-notes/67` on the same build. Eight real issues surfaced across the first
real build attempts, in order: five in getting the device tree recognized and configured at all
(quick, mechanical), then three more deep in actual compilation (this note's main content). All
diagnosed from direct evidence, not guessed.

## Five bugs getting from "device tree drafted" to "device tree actually builds"

Not yet written up anywhere until now — found and fixed inline during the first `make -C
components/os lunch` attempts, in order:

1. **`add_lunch_combo` is obsolete** on this AOSP version (`vendorsetup.sh`) — `COMMON_LUNCH_CHOICES`
   in `AndroidProducts.mk` already covers it; the file is now deliberately empty with a comment
   explaining why, kept only so the usual `source device/oculus/monterey/vendorsetup.sh` convention
   still finds something there.
2. **Lunch combos need the newer three-part `<product>-<release>-<variant>` form**, and the release
   name is this manifest's own `ap2a`, not AOSP's generic example release `trunk_staging` (the error
   itself named the valid option: `Available releases are: ap2a`).
3. **`PRODUCT_MAKEFILES` needs `<name>:<path>`**, not a bare path, on this AOSP version — confirmed
   by comparing against a real in-tree device (`device/google/cuttlefish/AndroidProducts.mk`) after
   the bare-path form silently resolved to nothing (`Cannot locate config makefile for product
   'lineage_monterey'`).
4. **The device tree was invisible to Soong's Finder entirely.** `device/oculus/monterey` was
   originally a single directory symlink into `components/os/device-monterey`; Soong's Finder
   (`build/soong/finder/finder.go`) only follows symlinked *files*, not symlinked *directories*,
   unless `FollowSymlinks` is set (default off, a deliberate anti-infinite-loop safeguard) —
   confirmed directly in that file's source, not inferred. Fixed by making
   `device/oculus/monterey` a real directory with each individual file symlinked back
   (`cp -rs`, `components/os/Makefile`'s `device-tree-link` target) instead of symlinking the whole
   directory.
5. **`dex_preopt_check.mk` failed hard** on missing `.odex`/`.vdex` artifacts for
   `org.lineageos.platform` once the tree was recognized and building — a real check, just not
   relevant to Phase 2's "boot to a shell" milestone. Fixed with `WITH_DEXPREOPT := false` in
   `device.mk`.

With those four fixed, the device tree reached real compilation — confirmed by 100% of legacy Make
module parsing completing cleanly. Three more issues surfaced from there, all deep in actual
compilation rather than configuration.

## Bug 1: `-j16` (the host's full thread count) caused a genuine host-wide OOM kill

At 72% (105876/146469 targets), the build stopped advancing. `podman stats`' aggregate CPU%/PIDs
looked identical for many minutes, which was first misread as buffering (a real phenomenon seen
earlier in this same build, ninja doesn't flush progress lines to a non-tty pipe promptly) rather
than a genuine stall. It was not buffering this time — `podman top` showed the actual child
processes were zombies with zero accumulated CPU time. Confirmed the real cause directly:

```
journalctl -k: gnome-shell invoked oom-killer ... task=ninja ... Out of memory: Killed process ... (ninja)
```

A real, host-wide OOM kill (`global_oom`, not container-scoped) — triggered by desktop memory
pressure (gnome-shell) competing with the build's own memory use, not a hard "this build needs more
than 30GB" ceiling. `-j16` means up to 16 concurrent build actions, and several of ninja's actions
here are JVM processes (kotlinc, R8, javac) that can each spike a few GB — 16 of those concurrently
is genuinely a lot, independent of total system RAM.

**Fix**: `components/os/Makefile`'s `lunch` target now defaults to `BUILD_JOBS=8`, not `$(nproc)`,
overridable per-host (`make -C components/os lunch BUILD_JOBS=N`). Retried at `-j8`: memory pressure
was still real (swap filled to 8.0Gi/8.0Gi at one point) but the system found a stable equilibrium
rather than OOM-killing again — slower due to heavy swapping, but it kept making genuine forward
progress (verified by checking individual process CPU time growth over multiple samples, not just
trusting the aggregate `podman stats` percentage, which turned out to average-over-lifetime rather
than report instantaneous load and was misleading on its own).

## Bug 2: git-lfs content wasn't actually downloading, just the filter was registered

The `-j8` retry ran cleanly all the way to **91%** (37322/40596 remaining targets after resuming
from the OOM point) before hitting a real, different failure:

```
FAILED: out/target/product/monterey/obj/APPS/webview_intermediates/package.apk
... cp "external/chromium-webview/prebuilt/arm64/webview.apk" ...
... zip2zip -i out/.../package.apk ...
zip2zip.go:82: zip: not a valid zip file
```

Traced directly to the exact four Git-LFS projects fixed earlier (`research-notes/67`,
`external/chromium-webview/prebuilt/{arm,arm64,x86,x86_64}`). Checked the actual file: 134 bytes,
`file` reports plain ASCII text, contents are a literal, un-smudged LFS pointer
(`version https://git-lfs.github.com/spec/v1`, `oid sha256:...`, `size 263298710`). **`git-lfs
install --system` (added to the container image in `research-notes/67`) registers the smudge
*filter*, but that alone does not guarantee `repo sync`'s checkout actually invokes it successfully
and fetches the real object** — it checked out without any error at sync time (which is why this
wasn't caught in `research-notes/67`), and only surfaced 91% into a build, roughly 40 minutes later,
when something finally tried to treat the 134-byte pointer text as real zip/APK content.

**Fix**: explicit `git lfs pull` per affected project directory, run directly and confirmed working
(`file` now reports "Android package (APK), with AndroidManifest.xml" for all four, and the arm64
one's byte count — 263298710 — matches the pointer's own declared `size` exactly). Added a new
`lfs-pull` target to `components/os/Makefile`, wired into `sync` so future syncs on a fresh checkout
don't hit this silently 40 minutes into a build again.

## Bug 3: hex partition sizes broke Python's decimal-only int parser, at 92%

The `-j8` + LFS-fixed retry ran cleanly all the way to **92%** (3028/3270 remaining targets) before
a third, different, genuine failure — this one actually in our own device tree, not the build
environment:

```
File "verity_utils.py", line 68, in CreateVerityImageBuilder
ValueError: invalid literal for int() with base 10: '0xA0000000'
```

`BoardConfig.mk` set `BOARD_SYSTEMIMAGE_PARTITION_SIZE := 0xA0000000` (the exact form
`fastboot getvar` printed, `research-notes/65`) — a completely normal way to write a BoardConfig
size, and most of the toolchain (mkbootimg, the make-level arithmetic) accepts hex `0x...` forms
fine. This one specific consumer, `build_image.py`'s `verity_utils.py`, parses the field with
`int(s, base=10)` — explicit base 10, which rejects a `0x` prefix outright rather than
auto-detecting it (`int(s, 0)` would have accepted it). It's invoked regardless of
`BOARD_AVB_ENABLE=false`; disabling AVB doesn't skip this code path, just changes what it does
afterward.

**Fix**: `BOARD_BOOTIMAGE_PARTITION_SIZE`/`BOARD_SYSTEMIMAGE_PARTITION_SIZE` changed to decimal
(`67108864`/`2684354560`), with the original hex form kept in a trailing comment for traceability
back to the `fastboot getvar` capture. `BOARD_KERNEL_BASE` left as hex (`0x00000000`) — nothing
downstream of it was shown hitting this same parser, and changing what isn't broken risked
introducing a new, harder-to-attribute difference from the reference trees for no reason.

## What this changes

None of the three bugs were in the device tree's *logic* — `device/oculus/monterey`'s actual
configuration decisions (no vendor partition, prebuilt kernel, AVB off) have been correct since
`research-notes/67`. Two were build-*environment* issues (container resource limits, an incomplete
LFS fix) and one was a genuine but narrow device-tree formatting bug, each only reachable by a real,
sustained build attempt — none of the three would have been caught by config validation alone.
Retried after all three fixes; as of this note the build is running again from a clean product
config regeneration (triggered by the `BoardConfig.mk` edit).

## Open

- `BUILD_JOBS=8` was chosen as "half the host's threads," not derived from a real memory budget —
  it avoided a second OOM kill on this specific host and this specific build state, not proven
  optimal or guaranteed safe on a different host or with more concurrent desktop load.
- Whether `git lfs pull`'s need to run as an explicit second step (rather than `repo sync` alone
  being sufficient) is specific to this manifest/repo config or a more general `repo`+LFS
  interaction is unconfirmed — recorded as "what fixed it here," not a general claim about `repo`.
