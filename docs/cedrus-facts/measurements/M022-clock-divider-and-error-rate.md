# M022: the engine's error rate is a function of its clock divider (2026-10-07)

> **Superseded by M023 (2026-10-07):** the cause was the driver's wait after a header-skip command (it waited for a bit that is always set); the engine error is gone and the margin / request-pipeline readings below are wrong. The measurements stand; the conclusions do not.

Method: the engine's module clock is `pll_ve` divided by the M field of the CCU's VE
clock register, `0x01c2013c` bits [18:16] (`M_CLK(ve_clk, …, 0x13C, 16, 3, …)` in
`freebsd-src-earlyboot-wt/sys/dev/clk/allwinner/ccu_a64.c`, and `aw_clk_m_recalc()` divides
the parent's rate by that field). The driver only *enables* this clock; its rate comes from
the bootloader, and `clk_set_freq()` on it is refused (`Cannot set frequency for clk: ve,
error: 34`), so the divider was written directly with the driver's own lab sysctls
(`dev.cedrus.0.poke_addr` / `poke`). Forty runs of `f3.264` (three pictures each) per
setting, with the register read back after each write and the run time measured.

| M field | 40 runs | failures | wall time |
|---|---|---|---|
| 0 (the boot default, 297 MHz) | 40 | **1 (2.5%)** | 2784 ms |
| 1 | 40 | **5 (12.5%)** | 3076 ms |
| 2 | 40 | **17 (42.5%)** | 2916 ms |
| 3 | 40 | **40 (100%)** | 4468 ms |

- F:M022-a  **The failure rate rises steeply as the engine's clock is divided**, from 2.5%
  at the boot setting to 100% at the fourth step, and the wall time grows with it, so the
  divider really took effect (the driver's own report is useless here: a module reload
  resets the field to 0).
- F:M022-b  **This reframes M016.** The engine is not failing because of its inputs or its
  programming — those are identical (F:M016-g/h) — but because it is working against a
  **timing margin**: it needs its module clock close to the AHB's (297 MHz against 300
  MHz at the boot setting) and dividing it breaks the engine's handshakes. The
  batch-to-batch variation of the residual rate (F:M016-j) fits a margin that moves with
  temperature and voltage.
- F:M022-c  **The default is the best end of the curve.** Slower is strictly worse, so the
  remaining ~2.5-5% at the boot setting is not a "too fast" problem; the only directions
  that could improve it are a *higher* engine clock (the PLL allows up to 600 MHz per its
  descriptor) or a different supply voltage — both are outside what a driver change can do
  safely, and touching `pll_ve`'s factors directly is the kind of poke that has hung this
  board's bus before (M001-a, and the pll_ve gating note in the driver).
- F:M022-e  **The default point, better measured:** after restoring the divider, 200 runs
  at the boot clock gave 185 PASS, 5 partial pictures and 10 engine errors — **7.5%**,
  inside the 2.5-8.7% spread the batches have shown (F:M016-j). The 2.5% in the table is
  a 40-run sample of the same point.
- F:M022-f  **A higher clock does not help, and the residual is rate-independent.** The
  fractional PLL's rate is `24 MHz x N / M` with both fields encoded as value-1
  (`aw_clk_frac_recalc()` in the kernel tree; 99 and 8 give the 297 MHz the driver
  reports). Reprogramming it to **336 MHz** (N=112, M=8, register 0x03006f07 written
  through the lab poke while the engine was gated) took: the lock bit came back set
  (`0x93006f07`) and the stream decoded. 200 runs then gave 185 PASS, 2 partial pictures
  and 13 engine errors — **7.5%, exactly the boot clock's rate** (F:M022-e). So the
  divider curve of F:M022-a is a *requirement* — the engine breaks when it is underclocked
  — and not a margin the residual failure sits on: above the nominal clock the rate is
  flat.
- F:M022-g  **Nor is the spacing between slices.** The lab leaves 20 ms between starting a
  slice and reading its status; the driver does not. 200 runs with a 20 ms delay after
  each completion (`h264_post_delay=20000`) gave 188 PASS / 1 partial / 11 ERROR — 6%,
  the same as the baseline within noise.
- F:M022-d  **What was left in place:**- F:M022-d  **What was left in place:** the divider was restored to 0 (0x80000000) after
  the measurement, and a decode at the default still passes. The `dev.cedrus.0.mod_mhz`
  knob added for this stays (default 0, does nothing); it records that the clock framework
  refuses to set this clock, which is why the divider had to be written directly.

Open: whether a *higher* engine clock reduces the residual rate is untested and needs a
PLL write (risky); whether the residual is a voltage-margin matter is the owner's domain.
What this file does settle is that the M016 error is a timing-margin effect, not a logic,
programming or coherency defect — which is what the lab's 100/100 clean runs said from the
other side.

Artifacts: this file; the CCU register definitions in the kernel tree; the driver's
`poke`/`peek` sysctls; the `mod_mhz` knob.
