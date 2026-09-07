# Low persistence: no software path drives it — 2026-09-05

`notes/44` found that `msm_fb_persist_mode` maps to the DT command set `qcom,mdss-dsi-lp-mode-on`,
and that no Oculus panel defines that property — so the standard Android low-persistence hook is
inert. This closes out where it *is* driven. The answer appears to be "nowhere in software", which
is good news for the OS swap.

## Everything checked, and empty

| candidate | result |
|---|---|
| `qcom,mdss-dsi-lp-mode-on` in any panel DT | **absent** across the whole DT tree |
| panel on-command sequence (`dsi-panel-sdc-lightman-video.dtsi`) | no emission-duty or persistence command — see below |
| composer HAL blob strings (`persist\|emission\|duty\|strobe\|lowpers\|vrmode`) | **none** |
| runtime DSI command node under `/sys/kernel/debug/mdss_panel_fb0/` | **absent** — the nodes there are read-mostly config (`bl_max`, `clk_rate`, `te`, `ulps_*`, `dynamic_fps`, ...), no `dsi_cmd` |

The panel-on sequence is fully accounted for and contains nothing persistence-related:

```
FC 5A 5A / F0 5A 5A     Samsung manufacturer key unlock
DSC mode + PPS          Display Stream Compression enabled
CB 10, F7 03            horizontal flip
B0/F2/CB                porch adjust
53 20                   DCS Write Control Display: BCTRL on
51 FF                   DCS Write Display Brightness: max ("100nit" per the comment)
44 00 00 / 35 00        Set Tear Scanline / Set Tear On (V-blank) -- the vsync/TE signal
2A 00 00 05 9F          column address 0..1439
29                      display on
```

Brightness is set once at panel-on and TE is enabled for vsync. There is no emission-duty register
write, and nothing that could be toggled per-frame.

## Best-supported explanation

The panel is **natively low-persistence**: emission duty fixed in panel firmware. That is normal for
a purpose-built VR OLED part, and it fits every observation — a generic MDSS hook left undefined, no
vendor command path, and a Samsung "Dual Samsung video mode dsi panel" whose only brightness control
is a one-shot DCS write.

**Stated as best-supported, not proven.** Proving it needs a photodiode against the panel to measure
the emission duty cycle directly, which is external hardware this project does not have. The
falsifiable prediction is that measured emission occupies well under the 13.718 ms active scanout
window (`notes/44`) with no software involvement.

## Why this matters, and it is good news

`notes/18` step 6 lists "direct mode, lens distortion and reprojection all unbuilt" as an OS-swap
risk, and low persistence would normally sit in that list — a replacement compositor that failed to
reproduce it would produce visibly smeared motion, and the cause would be hard to isolate.

If persistence is panel-native, **a replacement stack inherits it for free**. Nothing in Monado has
to reproduce it, and it does not need to be ported. The corresponding risk is the inverse and much
smaller: that the panel-on command sequence must be reproduced faithfully on the new OS, since it is
where DSC, porch adjust and the flip are configured. That sequence is published in the kernel DT we
already have, so it is a transcription job rather than a reverse-engineering one.
