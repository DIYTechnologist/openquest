# Step 6A: the distortion mesh is baked and decoded; kernel reverted to stock — 2026-09-05

Three results: the UFS workaround in the memory is **wrong**, the "device is corrupt" screen is
**not** our kernel's fault, and step 6A's distortion question is **answered** — Meta's lens
distortion is a baked 33x33 RGB mesh, now fully decoded and exported.

## The UFS clock-gating workaround does not work

`quest-ufs-link-death` prescribed pinning `clkgate_enable=0` and `hibern8_on_idle_enable=0` at the
top of a long session. That was done before the worn capture. **The link died anyway**, ~910 s into
that boot:

```
ufs_qcom_check_hibern8: unable to get TX_FSM_STATE, err -110
ufshcd_host_reset_and_restore: Host init failed -110
ufshcd_print_pwr_info: gear=[1,1], lane[1,1], pwr[SLOWAUTO_MODE,SLOWAUTO_MODE]
```

Symptom to recognise: **load average ~25 with ~0% CPU and ~380% iowait**, `jbd2/sda10-8` plus a
dozen kworkers stuck in `D`. Recovery is still sysrq (`echo b > /proc/sysrq-trigger`); `adb reboot`
hangs because init blocks on unmount. The mitigation is downgraded from "prevents it" to "does not
prevent it"; the only reliable recovery is a forced reboot.

## Reverted to the stock kernel — as the falsification test

Twice in one day the link died on our instrumented kernel, which `notes/21` built for camera
`dynamic_debug`. That instrumentation serves **step 1 only**, not the critical path (X -> 4 -> 5,
plus 6A), so reverting costs nothing currently on the path.

Flashed `backups/boot-monterey/new-boot_magisk30.7.img` (md5 `e534bd75...`, verified against the
backup README before flashing — `notes/21` records a flash that went wrong precisely because this
step was skipped). Result:

```
before: Linux 4.4.205-perf+ #2 SMP PREEMPT Thu Sep  3 20:58:43 BST 2026   (ours)
after : Linux 4.4.205-perf+ #1 SMP PREEMPT Wed Jul 31 02:31:55 PDT 2024   (Meta's)
root: intact (magisk)   UFS at boot: gear 3, lane 2, FAST MODE   /data: clean, 45 G free
```

Clock-gating left at stock defaults (`1`/`1`) deliberately, so the reliability test measures the
stock kernel as shipped rather than a patched configuration. **Open question:** whether the link
still dies. Not yet answered — it needs hours of uptime.

## The "device is corrupt" screen is not our kernel, and cannot be signed away

**Correction.** I previously attributed this screen to Meta's `sysimgcheck` service, having seen it
launched from init with `slideshow warning/verity_red_1`. That was wrong: it exits with status 1 in
**0.004 s**, far too fast to draw anything, and the screen appears before the kernel runs at all.
It is the bootloader's warning.

**It is not caused by our kernel.** It appeared identically after flashing
`new-boot_magisk30.7.img` — the stock-kernel Magisk image this device ran for days. It is triggered
by any boot image not signed by Meta, i.e. by the unlocked bootloader. It became noticeable only
because the kernel work meant frequent reboots.

**Can we sign and install our own key? No.** Evidence from `fastboot getvar all`:

```
unlocked:yes        secure:yes        oculus-ext-version:2
avb/vbmeta variables: 0
```

- No `vbmeta` and no `avb_custom_key` partition (`by-name` has only `boot_a`, `boot_b`, `devinfo`,
  `keystore`). `fastboot flash avb_custom_key` is an **AVB 2.0** feature; this device is **Verified
  Boot 1.0** — the kernel cmdline carries `veritykeyid=id:cc158dc3...` and a `dm="... android-verity
  /dev/sda6"` table, with none of AVB 2.0's `androidboot.vbmeta.*` parameters.
- `secure:yes` means the secure-boot fuses are blown: the root of trust is Meta's key in QFPROM,
  not replaceable.
- The only route to a non-warning state is re-locking, and re-locking a device whose images the
  bootloader will not verify is how it becomes a brick.

It costs one power press per boot and has nothing to do with the UFS wedge. Not worth pursuing.

## 6A: the distortion is a baked mesh — decoded

`/persist` has only panel decenter and luminance uniformity (`notes/05`), and neither
`libvrapi.so` (274 KB IPC shim), `vrapiserver` (212 KB), `libossdk.oculus.so`, nor the composer HAL
impl contains a single distortion string. It is **data, not code**:

**`/system/etc/calibration/distortion-mesh.bin`** — 52,368 B, layout confirmed byte-exact:

```
magic 0x56347807, version 257 / 259, grid 32 x 32 (cells)
u32 @0x30: 2880 1600 1216 1344      <- panel res, then per-eye render target
f32 @0x40: 47 53 52 42 / 47 53 42 52 <- FOV half-angles (deg), L/R mirrored

96-byte header + 2 eyes x 3 channels (RGB = chromatic aberration) x 33 x 33 verts x 2 f32
  = 96 + 52272 = 52368   exact match
```

So Meta corrects chromatic aberration with a **separate mesh per colour channel**, on a 33x33 grid
per eye. This is directly consumable: Monado takes a distortion mesh, so 6A task 3 becomes a format
conversion rather than a model fit. Vertex coordinates are not plain UVs (eye0 y spans
-1.837..-0.409, eye1 +0.253..+1.157) — the space is not yet characterised, and that is the
remaining work on this task.

**`/system/etc/display.conf`** — feeds 6A task 2:

```
DIRECTION 3 ("Bottom-to-top")   SHUTTER_TYPE 0 ("Rolling shutter")
SWAP_TIMING_FRONT_BUFFER   [0.5, 1.0, 1.0, 1.5, 1.5, 2.0]
SWAP_TIMING_SWAPPED_BUFFER [0.0, 0.0, 2.0, 2.5, 2.5, 3.0]
```

Two things worth having: the panel scans **bottom-to-top with a rolling shutter** (so reprojection
must account for scanout phase, it is not a global flash), and Meta ships an explicit
**front-buffer** timing table — direct evidence that the stock path does front-buffer/direct-mode
rendering. The values are in refresh periods; at 72 Hz (confirmed by
`/system/etc/device_props.json`, `device_default_refresh_rate: 72.0`) one period is 13.89 ms, so the
front-buffer path models 6.9–27.8 ms and the swapped path 0–41.7 ms. That is Meta's own swap-timing
model, and it is the natural cross-check for the motion-to-photon measurement in 6A task 1.

## Backup gap closed

`/persist/LLENS_SN` and `/persist/RLENS_SN` (both `1CHSL2AA030156`) are **per-unit and were not in
the `notes/05` export**, which covered only `/persist/{calibration,sensors}`. Now exported.
`/persist/display/` exists but is empty.

All of the above pulled to `exports/display-6a-2026-09-05/`.

## 6A status

| criterion | state |
|---|---|
| panel timing documented | **partly** — 72 Hz, rolling shutter, bottom-to-top, front-buffer table found; vsync behaviour not yet measured |
| distortion reproduced to a stated pixel error | **source located and decoded**; coordinate space still to characterise, no error figure yet |
| motion-to-photon measured | not started — but `SWAP_TIMING_*` now gives a model to check against |
