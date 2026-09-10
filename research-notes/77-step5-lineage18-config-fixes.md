# Step 5 — lineage-18.1 bring-up: two real config bugs found and fixed, real ninja build underway — 2026-09-09

Continuation of `research-notes/75`/`76`. The `repo sync` that was running in the background at the
end of the last session finished cleanly. Two real bugs found immediately after, both fixed;
`make -C components/os lunch-18` is now well into a real ninja build (31859 targets).

## Bug 1 — `lfs-pull-18` silently pulled nothing

`components/os/Makefile`'s `LFS_DIRS` variable was defined *inside* the `ifeq ($(IN_CONTAINER),1)`
block (used by the lineage-21 targets, which re-invoke `make` from inside their container with
`IN_CONTAINER=1` set). The `lfs-pull-18` target, by contrast, calls `podman run` directly from the
**host** — so `$(LFS_DIRS)` was expanding empty at the point the host-side `make` built the podman
command line: the log literally showed `for d in ; do ... done`. Silent no-op, exactly the same
failure class as the lineage-21 tree's LFS trap in `research-notes/68` (pointer files checked out
instead of real blobs, no error until something much later tries to use them as real content).

Confirmed live: `external/chromium-webview/prebuilt/{arm,arm64,x86,x86_64}/webview.apk` were all
134-byte-class LFS pointer text files (`file` reported "ASCII text"), not real APKs.

**Fix**: moved `LFS_DIRS`'s definition to top level (before the `ifeq` block), so it's visible to
both the in-container lineage-21 recipes and the host-side lineage-18 recipes. Removed the now-
duplicate definition inside the `ifeq` block. Re-ran `make -C components/os lfs-pull-18` — all four
`webview.apk` now really are APKs (94MB/263MB/152MB/347MB, `file` confirms "Android package (APK)").

**Standing rule**: any time a new host-side (non-`IN_CONTAINER`) Makefile target references a
variable that was only ever exercised from inside a container before, check where that variable is
actually defined relative to conditional blocks — don't assume "it's used elsewhere in this file"
means it's visible everywhere in this file.

## Bug 2 — `hardware/qcom-caf/common/common.mk` doesn't exist on lineage-18.1

`device-monterey-18/lineage_monterey.mk` (written before this tree's actual source existed, per its
own "UNTESTED first draft" comment) copied the lineage-21 tree's
`$(call inherit-product, hardware/qcom-caf/common/common.mk)` line verbatim. First real `lunch-18`
attempt failed fast and cleanly:

```
device/oculus/monterey/device.mk:12: error: ... "hardware/qcom-caf/common/common.mk" does not exist.
```

Checked directly against the now-synced tree. **Surprising finding, opposite of the obvious guess**:
`hardware/qcom-caf/common/common.mk` exists at lineage-21.0's tip (verified:
`work/lineageos/hardware/qcom-caf/common/common.mk` is real) but does **not** exist at lineage-18.1's
tip — that repo (`android_hardware_qcom-caf_common`) only ships `os_pickup.mk`/`os_pickup.bp`/
`fwk-detect` at the lineage-18.1 branch head. `common.mk` was added to this repo *later*, by
lineage-21.0 — the reverse of the usual "newer AOSP drops legacy files" pattern.

**Fix**: since this bring-up build's only goal is proving the monolithic-ramdisk boot path
(`research-notes/74`), not any HAL functionality, dropped the inherit line entirely rather than
chasing a path that doesn't exist on this branch. `core_64_bit.mk` + our own minimal `device.mk`
(no camera/audio/telephony packages, matching `research-notes/75`'s stated scope) is enough.

After this fix, `lunch-18` got past product-config resolution, through Soong bootstrap, through the
full `Android.mk`/`Android.bp` inclusion scan (`436` files), and into the real ninja build
(`bootimage systemimage`, 31859 total targets) — currently in `external/boringssl` early in the
dependency graph as of this note. Genuinely building, not stalled; no further config errors so far.

## Next step on resume

Check whether the ninja build (`make -C components/os lunch-18`, logging to `/tmp/lunch18.log` on
the host, backgrounded via `nohup ... &`/`disown` — NOT tracked by any tool's background-task
notification system, since it was detached deliberately after the first two attempts) has finished,
succeeded, or hit a new config/compile error. `tail -f /tmp/lunch18.log` or re-`tail` it; check
`pgrep -af ninja` / `podman top <container>` to confirm it's still actually progressing rather than
stalled, using this session's established techniques (real CPU-time growth across two samples, not
just instantaneous stats). If it completes successfully, the actual `boot.img`/`system.img` this
produces is the next thing to flash-and-boot-test, following the `slot-unbootable`-aware recovery
discipline from `research-notes/73`/`quest-slot-unbootable-recovery`.
