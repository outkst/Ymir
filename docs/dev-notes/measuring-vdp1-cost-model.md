# Measuring the VDP1 cost model

A step-by-step guide to quantifying how badly `VDP::VDP1CalcCommandTiming` overshoots, and to
identifying which correction is worth making.

## Why measure first

The VDP1 command cost model estimates the cost of each command from its geometry alone, then
`VDP::Advance` spends that estimate against a cycle budget that has been multiplied by 4
(`m_VDP1CyclesShift = 2`). That 4x fudge factor exists because the estimate is too expensive. Nobody
knows by how much, or in which direction for which command types.

There are at least four independent candidate causes, and they call for completely different fixes:

| Suspected cause | Fix if confirmed | Fix if ruled out |
| --- | --- | --- |
| Clipped pixels charged in full | Share the rasterizer's clip early-outs | Ignore |
| End codes / transparent texels charged | Model end-code termination | Ignore |
| All pixels priced identically | Per-`DrawMode` cost weights | Ignore |
| Wrong setup:fill ratio | Recalibrate the 16-cycle command fetch | Ignore |

Tuning `m_VDP1CyclesShift` cannot fix any of them, because a single scalar cannot make one game faster
and another slower. Measure before changing anything.

## Step 1 — Configure a profiling build

The instrumentation is behind a CMake option, off by default. From the repo root:

```
cmake --preset Full-x86-64-AVX2-Clang-Release -DYmir_ENABLE_VDP1_PROFILING=ON
```

Substitute whichever preset you normally use (`cmake --list-presets` shows them all). Confirm the
option took effect — the configure output includes:

```
-- Ymir: VDP1 profiling ON
```

Then build:

```
cmake --build out/build/Full-x86-64-AVX2-Clang-Release --target ymir-sdl3
```

Use a **Release** build. A Debug build's rasterizer is slow enough to change which frames drop, which
is the exact behaviour under study.

> Keep your normal build directory separate from the profiling one. You will want to A/B against an
> uninstrumented binary later.

## Step 2 — Set up the emulator for a valid capture

Two settings matter. Both are in **Settings > Video**.

1. **Turn OFF "Threaded VDP1 renderer".** This is not optional. The command-side counters are written
   by the emulation thread and the raster-side counters by the render thread; with threading on, the
   two halves describe different frames and every ratio you compute is meaningless. The profiler
   window shows a red banner when this is wrong, and the CSV records it in the `threaded_vdp1` column
   so you can discard bad captures after the fact.

2. **Leave "Accurate VDP1 drawing timings" OFF** for your first capture, i.e. keep the default
   `m_VDP1CyclesShift = 2`. You are measuring the model as it ships. Capture the accurate setting
   separately as a second run.

Also turn off fast-forward, rewind, and frame skipping if you use them.

## Step 3 — Capture

1. Load a game that exhibits the framerate drop.
2. Open **Debug > VDP > Cost model profiler**.
3. Get to the scene where the drop happens and let it settle for a second or two.
4. Click **Start CSV capture**. The file lands in `<profile>/dumps/vdp1_profile_<game>_<stamp>.csv`.
5. Play through the problem scene for 10–30 seconds.
6. Click **Stop CSV capture**.

Capture a **contrasting pair** for every game: one recording of the scene that drops frames, and one
of a scene in the same game that runs at full speed. The difference between the two is far more
informative than either alone, because it controls for the game's general rendering style.

## Step 4 — Read the headline number

The profiler window shows an **overshoot ratio**: estimated pixels divided by pixels the rasterizer
actually wrote.

- **~1.0** — the model is roughly calibrated for this scene. The overshoot is elsewhere (look at the
  fetch cost share, step 6).
- **1.2–2.0** — meaningful overshoot. Worth fixing, but probably not the whole story.
- **>2.0** — the model is charging for more than twice the work being done. This is your problem.

Compare the ratio between your dropping scene and your smooth scene. If the dropping scene has a
markedly higher ratio, the cost model is the cause of the drop rather than a bystander.

## Step 5 — Attribute the excess

The "Where the estimated pixels went" table splits every charged pixel into one of four outcomes:

| Row | What it means | Fix it implies |
| --- | --- | --- |
| **Actually plotted** | Written to the framebuffer. Real work. | — |
| **Discarded: clipped** | Rejected by the system/user clipping test. | Give the estimator the rasterizer's clip early-outs. Cheapest high-value fix. |
| **Discarded: transparent / end code** | Transparent texel, or the line hit an end code. | Model end-code termination and transparent-pixel skip. |
| **Discarded: mesh / interlace** | Mesh checkerboard or double-interlace line select. | Halve the cost of mesh-enabled commands. |

Whichever discard row is largest is the fix to make first. If **clipped** dominates — which is the
expectation for 3D-heavy games throwing geometry at the screen edges — then the single highest-value
change is to stop charging for off-screen pixels.

The line underneath reports quad scanlines stepped by the estimator versus by the rasterizer. A large
gap confirms the same diagnosis at the scanline level: `quadTiming` has no equivalent of the
rasterizer's `plottedSegmentsCount` early-out, so entirely off-screen quads are billed in full.

## Step 6 — Check the setup:fill balance

The summary table's **fetch cost share** is the fraction of estimated cycles that come from the flat
16-cycles-per-command fetch rather than from pixels.

- **High (>50%)** in a scene with thousands of small sprites: the frame is **setup-bound**. The 16-cycle
  constant is doing the damage, not the pixel rate.
- **Low (<10%)** in a scene with large polygons: the frame is **fill-bound**. The pixel rate is what
  matters.

If your dropping scenes are setup-bound and your smooth scenes are fill-bound (or vice versa), that
alone proves no single `m_VDP1CyclesShift` value can satisfy both — and justifies replacing the model
rather than retuning it.

## Step 7 — Check per-command-type pricing

The "Estimated cost by command type" table gives average estimated pixels per command, bucketed. Watch
for buckets whose average cost is implausible relative to what the command actually does — for example
`ScaledSprite` charging destination area for a heavily shrunk sprite that hardware would cover with far
fewer texel fetches.

Cross-reference **texel fetches** against **pixels plotted**:

- `texelFetches` >> `pixelsPlotted`: sprites are being shrunk. Hardware's high-speed shrink halves the
  fetches; the model doesn't know about it.
- `fbBlends` a large share of `pixelsPlotted`: lots of half-transparency/shadow work, which is more
  expensive on hardware than the model's flat rate. This is a case where the model *undershoots*.

## Step 8 — Analyse the CSV

Per-frame numbers are noisy. The window's averaging slider smooths the live view, but real conclusions
come from the CSV. A starting point:

```python
import pandas as pd

df = pd.read_csv("vdp1_profile_yourgame_1234567890.csv")
assert (df.threaded_vdp1 == 0).all(), "capture is invalid, threaded VDP1 was on"

df["overshoot"]    = df.est_pixels_total / df.pixels_plotted
df["clip_share"]   = df.pixels_clipped / df.est_pixels_total
df["trans_share"]  = df.pixels_transparent / df.est_pixels_total
df["fetch_share"]  = df.cmd_total * 16 / df.est_cycles_total
df["quad_waste"]   = 1 - df.raster_quad_lines / df.est_quad_lines

print(df[["overshoot", "clip_share", "trans_share", "fetch_share", "quad_waste"]].describe())

# Frames where the command list was cut short are the ones actually dropping.
print("incomplete frames:", (df.completed == 0).sum(), "/", len(df))
```

The `completed` column is the direct symptom: it is 0 when the command list was terminated by a write
to `ENDR` rather than reaching an End command. Correlate `completed == 0` against the other columns —
whatever is elevated in those frames is what is costing you the framerate.

## Step 9 — Validate a proposed fix

Once you have a candidate change to the cost model:

1. Re-capture the same scenes with the change applied and compare overshoot ratios.
2. Check `docs/dev-notes/finicky-games/vdp1-command-timings.txt` — **Virtua Racing** and
   **Dragon Ball Z: Shinbutouden** break if VDP1 commands are processed *too quickly*. Any change that
   reduces estimated cost risks regressing them. They are your lower bound.
3. Check `docs/dev-notes/finicky-games/vdp1-sensitive-video-timings.txt` for the erase/swap-sensitive
   titles. Those are sensitive to *when* drawing finishes relative to the swap, so a cost model change
   moves them too.
4. Run `ymir-sandbox`'s VDP1 accuracy harness (`runVDP1AccuracySandbox`) to confirm rendering output is
   unchanged — the cost model must not alter what gets drawn, only when.

## What the instrumentation does not tell you

This measures the emulator's estimate against the emulator's own rasterizer. That gives you the
*internal inconsistency* — pixels charged but never drawn — which is real, actionable, and almost
certainly a large share of the overshoot.

It does **not** give you the true hardware cycle cost per pixel. Grounding the model in absolute terms
needs measurements from real hardware: a test ROM that draws a known command list and times it against
`HCNT`/`VCNT` or the sprite-draw-end interrupt. Until then, treat the ratios here as relative
corrections, and keep `m_VDP1CyclesShift` as the global calibration knob it already is.

## Files touched by the instrumentation

- `libs/ymir-core/include/ymir/hw/vdp/vdp1_profiler.hpp` — counters, CSV writer, all hook macros
- `libs/ymir-core/src/ymir/hw/vdp/vdp.cpp` — command-side hooks
- `libs/ymir-core/src/ymir/hw/vdp/renderer/vdp_renderer_sw.cpp` — raster-side hooks
- `apps/ymir-sdl3/src/app/ui/windows/debug/vdp1_profiler_window.{hpp,cpp}` — live view

All of it compiles to nothing when `Ymir_ENABLE_VDP1_PROFILING` is off, which is the default. The
whole thing is designed to be deleted in one commit once the cost model is fixed.
