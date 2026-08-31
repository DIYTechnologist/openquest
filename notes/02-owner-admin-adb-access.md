# Quest 1 Owner-Admin ADB Access Record

Date: 2026-08-30
Device serial: `1PASH9ACHD0215`
Device: Quest 1 / `monterey`
Purpose: temporary owner-admin ADB access for local partition backup and OS-porting work.

## Scope

This records an owner-authorized local maintenance session on one owned headset. The goal was
to obtain a temporary `uid=0` Android shell long enough to preserve device-specific
partitions and hardware-description data. No third-party systems, services, accounts,
credentials, networks, or content protection are in scope.

## Starting State

The headset was connected over USB ADB and booted into stock Android:

| Check | Result |
|---|---|
| ADB serial | `1PASH9ACHD0215` |
| Model | Quest 1 / `monterey` |
| Active slot | `_a` |
| Build incremental | `49845030443200410` |
| Kernel | `4.4.205-perf+` |
| Bootloader lock property | `ro.boot.flash.locked=0` |
| Initial ADB shell | `uid=2000(shell)` |

## Tool Used

The temporary admin step used the Quest 1 `ionstack` binary from
`darknight1050/quest1-bootloader-unlocker-web`.

Verified binary details:

| Field | Value |
|---|---|
| Source path | `binaries/quest1/ionstack` |
| Local staging path | `/tmp/quest1-ionstack` |
| Device staging path | `/data/local/tmp/q1u/ionstack` |
| Size | `258136` bytes |
| SHA-256 | `d65c800a2a032d3af579b8c9e5e11c12ae9b344d95c545396f1c83ccc6d4fc47` |

The SHA-256 matched both the prior successful run log and the upstream
`binaries/EXPECTED.sha256` manifest.

## Procedure Followed

1. Confirmed the device was visible with ADB.
2. Confirmed the current shell was not already elevated: `uid=2000(shell)`.
3. Confirmed the device was on slot `_a` and build `49845030443200410`.
4. Downloaded the Quest 1 maintenance binary and verified SHA-256 locally.
5. Created `/data/local/tmp/q1u` on the headset.
6. Pushed the verified binary to `/data/local/tmp/q1u/ionstack`.
7. Set mode `755` and re-verified the on-device SHA-256.
8. Started the binary with the Quest 1 tuning parameters used by QuestStack.
9. Watched `/data/local/tmp/q1u/ionstack.log` for success markers.
10. Confirmed a fresh ADB shell returned `uid=0(root)`.
11. Confirmed SELinux was `Permissive` for the maintenance session.

## Result

Current verification from the live session:

```text
uid=0(root) gid=0(root) groups=0(root),1004(input),1007(log),1011(adb),1015(sdcard_rw),1028(sdcard_r),3001(net_bt_admin),3002(net_bt),3003(inet),3006(net_bw_stats),3009(readproc),3011(uhid) context=u:r:shell:s0
Permissive
```

The log contained the expected success markers:

```text
ROOT VERDICT root=1 uid=2000->0 selinux_permissive=1 (1->0) persist=1(adbd)
xrw: ROOT held -- box staying up, adbd is root, `adb shell` gives root. Not tearing down.
```

## Operational Notes

- This is temporary. A reboot clears the elevated ADB state.
- Leave the running `ionstack` process alone while using the elevated session; it is holding
  the temporary maintenance state.
- To exit cleanly after backup work, reboot the headset rather than killing the process.
- If the session is needed again later, repeat the verified-binary staging and run procedure
  instead of rerunning the full bootloader-unlock workflow.

## Work Completed During This Session

The following high-value data was backed up after owner-admin ADB access was established:

- `vision`
- `persist`
- `private`
- active `boot_a`
- `system_a` and `system_b`
- compressed live `userdata`
- every remaining by-name partition as a raw `.img`
- live unlock-capable slot-B service chain
- raw FDT as `fdt.dtb`
- `/sys/firmware/devicetree/base` as `devicetree.tar`
- first/last 1 MiB GPT/header captures for UFS devices `sda` through `sdf`

Backup directory:

```text
backups/1PASH9ACHD0215-2026-08-30T15-20-root/
```

Hashes and sizes are recorded in that directory's `MANIFEST.md`.
