# Where the human changed the outcome — 2026-09-04

A record of the points in this project where human input redirected the work: pushback that
overturned a wrong conclusion, reframes that unblocked me, and risk calls I would not have made.
Written for a post on human/LLM collaboration, so it is specific and quoted rather than
complimentary, and it includes the cases where the pushback did **not** pan out.

Quotes are verbatim, typos included.

---

## 1. The big one: I gave up on the camera path, and was wrong

**Context.** Step 1 (B2) meant driving the Quest's four tracking cameras directly through the kernel
with no Meta userspace blobs. It is the load-bearing piece of the whole project: `notes/16`
established the OS swap deletes `/vendor` entirely, so anything built on Meta's camera libraries is
non-inheritable. After five debugging sessions I invoked the kill criterion from my own plan and
committed:

```
ecac8f3  B2 parked at the kill criterion; B1 remains the working camera path

  Implemented both ordering changes plus a third fix found on the way. None produced frames, so per
  the notes/18 kill criterion this is parked rather than ground on further.
  [...]
  Pipeline still comes up cleanly, every ioctl returns 0, kernel log clean, no frames.
```

I was not lightly giving up — I had a documented criterion and I hit it. I also recorded what I
believed was the strongest remaining lead (a CSID version writeback discrepancy). **That lead was
itself wrong**, which matters: my confidence about *where* the bug was, was misplaced at the same
time as my conclusion that it was unreachable.

**The human's response, in full:**

> keep digging

> this should be solvable, the vendor does it

**Why that was the right call.** "The vendor does it" is an *existence proof*. My kill criterion
measured **my search effort**, not whether the thing was possible — and I had silently conflated the
two. Once framed as "this is definitely reachable, so your search is wrong", the question stops
being "should we stop?" and becomes "which of your assumptions is false?"

**What it unlocked.** Two bugs, both mundane:

1. The MCU streams **nothing** unless `camera_init`/`start_streaming` are told `num_cams=4`. Every
   B2 run had used `ncam=1`, so no sensor ever emitted a single MIPI packet. Five sessions of
   pipeline debugging found nothing wrong with the pipeline *because nothing was wrong with it*.
2. `ispif_call()` hardcoded `intftype = RDI0`, so cameras 1 and 3 silently overwrote 0 and 2's
   routes. Invisible until bug 1 was fixed.

**Result:** step 1 went from parked to **5/5 criteria complete**, and the open camera path now runs
sensor-to-6DoF at 0.203 m final drift — better than the 0.56 m baseline, which had gone through
Meta's blobs.

**The generalisable lesson is narrower than "don't give up".** It is: *a kill criterion based on
search effort is not evidence about feasibility, and an existence proof should override it.* I had
the existence proof available the whole time — the vendor's own stack was running on the same
hardware — and did not weight it correctly.

**My own contribution to the fix, for balance:** what actually found the bugs was a method change —
running the *working* control through the identical instrumentation and diffing, rather than
instrumenting the failing path alone. That was mine, and it took one run each after five sessions of
the wrong approach. The human bought the time; the method found the bug.

---

## 2. "One change at a time" — after I broke the device

**Context.** Flashing a self-built instrumented kernel to the headset. It bootlooped.

**Root cause was my methodology, not the kernel:** I changed **two things at once** — a new repack
chain *and* a new kernel — so when it failed I could not tell which was at fault. (The actual cause
was a missing `LEGACYSAR` hexpatch in the repack, not the kernel at all.)

**The human, after we recovered the device:**

> ok try again this time one change at a time

**What changed.** The next attempt began with a **no-op validation**: repack the *stock* kernel
through the new chain and check it reproduced the known-good image byte-identically (md5
`2240bd8e…`). It did. Only then was the new kernel introduced. It booted.

This is now a standing rule in `notes/24`. Worth noting the correction was about **process, not
domain knowledge** — the human did not need to know anything about Android boot images to identify
the error, and I did need telling despite knowing the principle perfectly well in the abstract.

---

## 3. Gating irreversible actions

> ok build the kernal but dont flash until we have discussed

I was ready to proceed to flashing. The human inserted a checkpoint before the one genuinely
device-bricking step. This is the clearest case of a human supplying **judgment about
irreversibility** rather than information.

A related exchange showed the division of labour cleanly:

> we should flash to _a as i believe _b is the bootloader that is unlocked we can use to recover.

The stated reason was wrong — `abl`/`xbl` are the bootloaders, `_a`/`_b` are A/B slots, and unlock
is device-global. But **the conclusion was right**, for a different reason: the device was actually
booted from slot `_a`. The useful pattern is that the human's instinct pointed at the right action
and my job was to supply the correct reason — neither of us had the whole thing.

---

## 4. A question that overturned my diagnosis

**Context.** Mid-session, all writes to `/data` began hanging. I diagnosed it confidently as **f2fs
directory corruption** and said so.

**The human:**

> is the f2fs issue repairable?

A plain question, not a challenge. But answering it required actually checking, and the check
demolished the diagnosis:

- `/data` is **ext4**, not f2fs. Wrong filesystem.
- Mounted `rw`, zero filesystem errors. Nothing to repair.
- Real cause: the **UFS host controller link died during idle clock-gating** and could not recover
  (`hibern8 enter failed. ret = -110`).

I had produced a confident, specific, wrong diagnosis and would have carried it forward unexamined.

Two further self-corrections followed from continuing to look:

- I claimed "a reboot clears it — the device ran 45 minutes fine after the last one." **No reboot had
  ever happened.** Uptime showed 9541 s continuous; `adb reboot` was silently failing because init
  blocks on unmounting dead storage. I had asserted a recovery I never observed.
- The failure also meant `adb push` **reported success while writing only page cache**, so several
  runs in that window were silently invalid. Worth knowing before trusting any result from them.

**Where the human's follow-up hypothesis did *not* pan out — stated honestly:**

> i dont think you had that before with hte other non debug kernel

Plausible, and I could not dismiss it: our kernel config is byte-identical to stock except one
line, but the toolchain differs and the hibern8 handshake is timing-sensitive. **It remains
untested.** I recorded it as a falsifiable open question rather than adopting or rejecting it.

The honest takeaway: the *value* of that pushback was forcing a claim to be checked, not that the
specific hypothesis was correct. Pushback that turns out to be wrong is still doing work.

---

## 5. Two strategic reframes that changed the project shape

### Incremental replacement

> Can we replace each of teh meta services 1 by 1 with our own before we jump to installing the
> 'new os'

I had been treating the OS swap as the goal and the services as things to port afterwards. This
inverted it: replace services **on the stock OS first**, so each replacement is developed against a
working reference with a known-good fallback, and the eventual swap becomes a *port of proven code*.
That became `notes/17` and the entire execution plan in `notes/18`.

### Security as both driver and gate

I had written that security was a side-effect rather than a driver. The correction:

> its both, we need to move off the os to fix security BUT we cannot move until we know the services
> can be moved, incremental replacement builds them against the old version meaning when we move we
> are just porting but we have reference data and a known good to fall back to

This is a better articulation of the project's logic than the one I had written, and it is not a
detail — it explains *why* the incremental order is forced rather than merely convenient.

### Asking for the dependency graph

> (note dependancies of steps within the plan to allow us to work in parallel)

I would have produced a linear plan. Forcing an explicit dependency analysis surfaced that **steps 2
and 4 do not depend on step 1** — which mattered enormously, because step 1 was parked at the time.
Without that analysis, parking step 1 would have looked like blocking the entire project. It also
identified the true critical path (X → 4 → 5) and let step X run early, where it could have
invalidated step 4 cheaply.

---

## 6. Physical-world context I structurally lack

Two examples where the human simply had information I could not have:

> The headset is currently on a desk in a bright room can you capture a single image from the
> cameras and use that static image to gauge overlaps etc?

A cheap experiment answering a question I was treating as needing a full capture session. It became
`notes/15` (four-camera overlap, best pair, and the finding that every capture to date had been
saturating 11–47 % of pixels with the wrong exposure).

> its on, it turns the screens on i'm worried about image burn can you disable them?

I had asked for the proximity sensor to be covered so tracking would stay in 6DOF, and had been
content to leave a **static VR shell rendering on OLED panels** indefinitely. That is a real risk to
their hardware that I did not consider at all. It led to `display_off`/`display_on` helpers and a
new default of leaving the panel blanked.

The same message asked for something else I had not thought of:

> also just add a small check at the start to ensure its 'covered' before you make your captures

Implementing it surfaced a genuine trap: the check **must run before services are stopped**, because
`sys.hmt.mounted` stops updating once `trackingservice` is down — a check placed later reads a stale
value and passes with the cover off. That guard would have silently wasted a worn capture session.

---

## 7. Where I pushed back, or held a line

For balance — this was not one-directional.

- **I declined to modify `/persist`.** I found the lever that would permanently remove the
  proximity-cover dependency: the mount thresholds are plain text files read via
  `request_firmware()`. I did not change them. `/persist` holds every factory serial and calibration
  on the device — camera, display and lens serials, IPD limits, controller UUIDs, wlan MAC — and the
  saving was one piece of tape. I backed it up in full (14 MB) and recorded the lever unused.
- **I refused to report two acceptance criteria as failures** without measuring the control first.
  B2 scored 3.3–4.9 LSB against a "< 2 LSB" bar — but B1 scores **6.4–12.0 LSB against its own
  frames**. The threshold was below the sensor noise floor and unmeetable by anything. Similarly the
  "< 200 µs" sync criterion turned out to measure interrupt latency, not sensor sync, because both
  cameras on a VFE share one IRQ timestamp.
- **I corrected the A/B slot reasoning** while agreeing with the conclusion (§3).
- **I killed my own false lead three times.** I twice claimed CSID status `0x800` was evidence of
  absent CSI data. It appears identically in runs that deliver 302 frames per camera. It is
  reset-done and never carried information about data flow.

---

## Patterns

1. **An existence proof outranks a search-effort kill criterion.** My stopping rule measured how
   hard I had looked, then got reported as a conclusion about feasibility. "The vendor does it"
   should have dominated.
2. **Confidence and correctness came apart in a specific way.** When I parked B2 I was wrong about
   the conclusion *and* wrong about where the bug was — but the write-up read as careful and
   well-evidenced, because it was. Calibrated-sounding prose is not calibration.
3. **Process corrections needed a human even where I knew the principle.** "One change at a time" is
   something I would have stated as obvious if asked, and violated anyway under momentum.
4. **Plain questions are powerful.** "is the f2fs issue repairable?" was not a challenge and
   demolished a wrong diagnosis simply by requiring the claim to be checked.
5. **Wrong pushback is still useful.** The instrumented-kernel/UFS hypothesis was never confirmed.
   It still forced verification and produced a recorded, falsifiable open question.
6. **The human held the physical and irreversible domain.** Burn-in, "don't flash yet", the state of
   the room, whether the cover was on. None of it was available to me, and some of it (burn-in) I
   would not have thought to ask about.
7. **Cheap experiments were consistently under-proposed by me.** The static-image capture and the
   B1 control run were both minutes of work that replaced hours of speculation, and both were
   prompted rather than volunteered.
