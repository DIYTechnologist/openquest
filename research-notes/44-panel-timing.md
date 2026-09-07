# Panel timing measured — 6A task 2 — 2026-09-05

Measured on the live panel plus the published panel DT, rather than taken from spec sheets.

## Refresh: 71.819 Hz, not 72

120 vsync timestamps read from `/sys/class/graphics/fb0/vsync_event` with the display awake
(proximity bypass, `notes/34`). Intervals above 20 ms are dropped: they are missed sysfs reads and
appear as exact multiples, not as real long frames.

```
80 single-frame intervals
mean 13.9239 ms   median 13.9237   sd 3.4 us   min 13.9125   max 13.9321
-> 71.819 Hz   (nominal 72 Hz = 13.8889 ms, so +35.0 us per frame)
```

Extremely stable — **3.4 us standard deviation** — but consistently 0.18 Hz below nominal. Anything
that predicts a photon time by counting whole frames from a vsync will accumulate 35 us per frame,
i.e. ~2.5 ms over a second. Worth carrying in the reprojection maths rather than assuming 72.000.

## Panel and scanout geometry

`/sys/class/graphics/fb0/` plus `dsi-panel-sdc-lightman-video.dtsi`:

```
type            mipi dsi VIDEO panel  (not command mode: continuous scanout, no self-refresh)
panel           "Dual Samsung video mode dsi panel", dual_dsi=1, split 1440 + 1440
mode            2880x1600 @ 72          per link 1440x1600
virtual_size    2880 x 3200             = two buffers, so double-buffered at the fb level
h porch/pulse   fp 250  bp 110  pw 70   -> htotal 1870
v porch/pulse   fp 10   bp 6    pw 8    -> vtotal 1624
pixel clock     218.11 MHz per link at the measured rate
dynamic fps     enabled, mode 3, range 60-72 Hz
HDR             is_hdr_enabled=1, peak_brightness 1000000, blackness_level 50
```

Derived, and this is the number that matters for reprojection:

```
active scanout 13.718 ms = 98.5 % of the 13.924 ms frame;  vblank only 0.206 ms
-> the top and bottom of the panel are 13.72 ms apart in time
```

Combined with `display.conf` (`notes/32`): `DIRECTION=3` bottom-to-top, `SHUTTER_TYPE=0` rolling
shutter. So the display is **not** a global flash — it is a near-continuous 13.7 ms sweep from the
bottom of the image to the top. A replacement compositor that reprojects a whole frame to a single
pose will be wrong by up to 13.7 ms of head motion at the extremes.

## Low persistence: the standard hook is inert here

`msm_fb_persist_mode` exists and reads 0. In the driver it maps to
`MDSS_PANEL_LOW_PERSIST_MODE_ON/OFF`, which issues the DT command set
`qcom,mdss-dsi-lp-mode-on` (`mdss_dsi_panel.c:2226`).

**No Oculus panel dtsi defines that property** — grep across the whole DT tree returns nothing. So
the generic Android low-persistence path does nothing on this device, and persistence must be
achieved elsewhere: in the panel's own firmware, in Meta's composer HAL, or through emission control
not exposed here. Recorded as an open question rather than guessed at; it matters because low
persistence is what makes VR motion look sharp, and a replacement stack has to reproduce it.

## Meta's own latency model, in milliseconds

`SWAP_TIMING_*` from `display.conf` converted at the measured frame period:

```
FRONT_BUFFER   [0.5, 1.0, 1.0, 1.5, 1.5, 2.0] periods =  6.96, 13.92, 13.92, 20.89, 20.89, 27.85 ms
SWAPPED_BUFFER [0.0, 0.0, 2.0, 2.5, 2.5, 3.0] periods =  0.00,  0.00, 27.85, 34.81, 34.81, 41.77 ms
```

The existence of a front-buffer table is direct evidence the stock path does front-buffer / direct
rendering, and these are the figures the eventual motion-to-photon measurement should be checked
against.

## 6A status

| criterion | state |
|---|---|
| panel timing documented | **done** — refresh 71.819 Hz (sd 3.4 us), video mode, dual DSI, scanout 13.718 ms bottom-to-top, vblank 0.206 ms, double-buffered, dfps 60-72 |
| distortion reproduced to a stated pixel error | source decoded and parsed (`notes/42`); conversion mechanical, error figure needs a comparison render |
| motion-to-photon measured | not started; model above gives the expected values |

Open: how low persistence is actually driven, since the standard hook is unused.
