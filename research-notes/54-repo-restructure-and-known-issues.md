# Repo restructure: components/tools/docs split, containerized builds — 2026-09-07

Status report. The repo was reorganised so the four working replacement binaries are maintainable
independently of the reverse-engineering scratch work that produced them, per user direction
(not gated on new research — no device time used).

## What changed

- **`components/{camera,controllers,tracking,kernel}/`** — one directory per Meta service being
  replaced, each with its own `Makefile`, independently buildable (`cd components/tracking && make`
  works standalone, same as `make -C components/tracking` from the repo root).
- **`tools/`** — everything else: RE/diagnostic tooling and offline research scripts, unmoved.
- **`notes/` → `research-notes/`** (this file), contents unchanged, history preserved via `git mv`.
- **`docs/`** — one file per component (what it replaces, protocol, build/run, status, limits) plus
  an index, distinct from `research-notes/`'s chronological log.
- **`build/containers/`** — three Dockerfiles (Android NDK r27c, the kernel's AOSP GCC 4.9
  toolchain, the OpenVINS+OpenCV+Boost+Eigen bundle) so no component needs anything beyond `git` and
  `podman` on the host. `build/mk/container.mk` is the generic re-exec-into-container Makefile
  pattern every component's `Makefile` includes.
- Compiled binaries and generated kernel-header staging directories (`.kinc`/`.kinc2`) that were
  previously committed are now `.gitignore`d and removed from tracking — builds are reproducible
  through `make`, so committing their output was redundant and could go stale silently.

## Verified, not just written

All four components built real, working artifacts through their containers from a clean checkout
(nothing but `git`+`podman` on the host, no NDK/toolchain/OpenCV pre-installed):

- `components/camera` → `cam_kernel`, valid aarch64 Android PIE binary.
- `components/controllers` → `sb_leech`, valid aarch64 Android PIE binary.
- `components/tracking` → `vio_live`, `pose_inject`, `replay_feed`, `ov_bench`, all valid aarch64.
- `components/kernel` → a real `Image.gz-dtb`; the embedded `Linux version` string reports
  `gcc version 4.9.x 20150123 (prerelease)`, byte-identical to the compiler string `research-notes/21`
  recorded from the device's own `/proc/version` — confirms the container's pinned toolchain is the
  right one, not just "a" 4.9 toolchain.
- `make clean` works uniformly across all four from the top-level `Makefile`.

Re-verified `camera` and `controllers` a second time after the review below with no changes in
between; `kernel` and `tracking` were not rebuilt a second time since nothing in their inputs
changed, and each already produced a working artifact in the pass above.

**Not verified:** on-device execution. The Quest was unreachable both times (no route to host on
the known wireless address) — worth an adb smoke test of at least one binary per component once
it's back on the network, per the standing verification bar this project holds itself to.

## Two real, pre-existing bugs found and fixed while wiring up the containers

Not reorg mechanics — both were live bugs in the scripts being moved, caught because running them
inside a fresh container surfaces failures a long-lived host environment had papered over:

1. **`components/kernel/build.sh` (was `tools/kernel-patches/build.sh`)**: built its `make`
   invocation as one flat string (`M="make ... HOSTCFLAGS=$KHOSTCFLAGS ..."`) and called it
   unquoted. `$KHOSTCFLAGS` contains several space-separated flags, so the unquoted expansion word-
   split it into separate argv entries; GNU make's getopt then parsed the fragment `-std=gnu89` as
   bundled short options and failed on the `=`. Fixed with a quoted `kmake()` wrapper function.
2. **OpenVINS commit pinning**: neither the new on-device dependency image nor the existing
   `tools/openvins-docker/Dockerfile` (host-side validation) pinned an OpenVINS commit — both did a
   floating `git clone` off whatever `rpng/open_vins` HEAD was on build day. This silently defeated
   the original intent (`tools/openvins-android/fetch.sh`'s own comment: "so the on-device numbers
   describe the same revision" as host validation). Both are now pinned to the same commit
   (`6948812`, matching the already-built `openvins:runner` image), so a rebuild of either can't
   drift from the other.
3. The AOSP GCC 4.9 prebuilt toolchain repos' default branch has since been emptied upstream (just
   an `OWNERS` file) — a plain `git clone` gets nothing usable. The container Dockerfile now fetches
   the exact two commits from the working `work/kbuild/` checkout that produced `research-notes/21`'s
   result, not a moving branch tip.

## Known issues — noted, not fixed

An independent review of the moved/adjacent code turned up five real, still-open issues. Recorded
here rather than fixed now:

1. **`components/camera/src/cam_kernel.c:781`** — `mcu_configure()`'s return value (which ORs
   together five `sb_send`/`sb_prop` results, including the IMU-enable packet) is discarded. A
   dropped IMU-enable leaves a capture with camera frames but no `0x50` IMU records, exiting 0 as if
   successful, discovered only when the dataset reaches `build_euroc_leech.py` or `vio_live` — after
   a worn-headset session is already gone.
2. **`components/camera/src/cam_kernel.c:792`** — partial camera bring-up (e.g. 3-of-4) isn't
   fail-fast: only `up == 0` aborts. The poll/DQBUF loop and teardown both iterate `i < ncam`
   unconditionally. Traced the actual failure paths: not memory-unsafe (`cams[]` is zero-init
   static, and stray fds fail `poll`/`ioctl` benignly), but a real silent-degradation + fd/ION leak
   bug — a partially-up run exits 0 with one camera's data simply missing.
3. **`tools/vio/build_euroc_leech.py:183`** — a short/missing frame read `continue`s past writing
   just that one camera's row, not the whole stereo pair, so `cam0/data.csv` and `cam1/data.csv` can
   silently diverge in row count and timestamps from one corrupt frame onward.
4. **`tools/vio/build_euroc_leech.py:141`** — `--imu-offset-ns` defaults to 0 and `--cam-shift-ns`
   defaults to one specific capture's fitted 254ms value, while `research-notes/51`/`53` both
   establish these must be measured per capture (research-notes/53 measured a *different* value,
   +109ms, on another capture, and running the wrong one produced multi-kilometre divergence).
   Should require explicit values or a named capture-id preset rather than silently defaulting.
5. **`tools/cam_direct/cam_direct.c:195`** — `SYNCBOSS_RAW=1` mode returns before the vendor lib is
   ever dlopened, so `sb.imu_enable` stays NULL for the whole process, and the raw-packet setup path
   (`sb_raw_setup()`) has no IMU-enable packet either (unlike `cam_kernel.c`'s `mcu_configure()`,
   which explicitly sends one). Raw-mode `cam_direct` captures silently lack IMU unless something
   else already enabled it. Lower impact since `docs/camera.md` already scopes `cam_direct` as the
   superseded B1 reference, not the critical path.

**One finding from the same review was already fixed** by this restructuring itself: previously
committed binaries/generated headers contradicting the README's stated "source and small text
artifacts only" policy — resolved by the `.gitignore`/`git rm` changes above.

## Scoreboard — unchanged by this session

This was pure repo organisation; no research was done. Per `research-notes/53`: step 2 (ground
truth vs Meta) is DONE — 7.6 cm in-place, 11.7 cm room-scale over a 60.6 m walk. Still open per
`docs/tracking.md` and `docs/controllers.md`: motion-to-photon latency (step 4's last criterion),
and controller 6DoF pose fusion location (step 3's last criterion, camera-IR-blob candidate
unquantified). Both are legitimate next items on the critical path; neither was touched here.
