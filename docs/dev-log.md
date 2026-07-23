# Mandelbrot Renderer — Development Log

A running log of the significant requests, diagnoses, and decisions made while
developing the multithreaded perturbation-theory Mandelbrot renderer (raylib GUI +
CPU engine). Organized chronologically by topic. Code line references reflect the
state at the time of each entry and may have moved since.

---

## 1. Initial review — perturbation rendering path

**Prompt:** "I completely revised PerturbationEngine.hpp, PerturbationJob.hpp and
added RebaseMatrix.hpp. Check them and find any faults about how they integrate
with the whole process." Later broadened to: "review now and try to isolate every
problem, functional or stuff that might be optimized off the Perturbation
Rendering."

**Findings (representative, not exhaustive):**
- Stale `#include "engine/PerturbationEngine2.hpp"` — wrong filename.
- `ReferenceCache::build` never grew `orbit` (`operator[]` used instead of
  `push_back`), so perturbation silently degraded to per-pixel BigFloat fallback
  on every frame.
- `thresholdEnvelope.reserve()` used where `.resize()` was needed — UB on OOB
  writes.
- Cooperative rebase barrier (`waitRebaseIteration`) never actually blocked —
  off-by-one in the iteration comparison, plus a data race reading
  `sharedMatrix.stepSize`/`center` without synchronization on the terminal pass.
- Cache reclamation was an unguarded UAF: `new` → `exchange` → immediate `delete`
  with no protection for stragglers still reading the old pointer.
- `rebuilds_` telemetry never incremented; glitched pixels painted magenta
  instead of being resolved.

**Fix direction:** rewrite the rebase→render handoff around a real
release/acquire "cache published" signal, replace immediate `delete` with
deferred reclamation (this is what eventually became the hazard-pointer work in
§9), fix orbit sizing, fix envelope sizing, add telemetry.

---

## 2. Stall diagnosis — "all threads but one pass OUTTA JOB"

**Prompt:** "Ok I added more prints and I can see that threads don't hang they
just take a long time to process the chunks, I want to optimize that... Actually
all threads but one pass the OUTTA JOB FLAG."

**Diagnosis:** Root cause was a **per-pixel BigFloat fallback storm**. The
reference-orbit probe was capped at 5000 iterations while the render budget could
be far higher; any reference that escaped between the cap and the true budget was
"short," so every non-escaping pixel fell back to a full `cpp_bin_float_50`
per-pixel Mandelbrot computation up to `max_iter`. One worker landed on the
heaviest tile and ground through this while everyone else drained fast tiles and
sat idle — the classic single-straggler signature.

**Fix:** lift the probe cap to the real budget, detect an inadequate reference
before rendering with it, and bound the fallback with a per-tile cap so a bad
reference degrades to a fast approximation instead of freezing a worker.

---

## 3. Low-res preview performance and correctness

**Prompt:** "the fact is that i'd want this rendering to be fast because it's
highly related to user input" → "Still the low resolution frame rendering is so
slow and botched" → "the low resolution frames are still messy... when panning a
little bit the image may change so much."

**Sequence of fixes:**
1. **Cheap probe, not raw screen center.** Using the raw camera center as the
   reference was fast but wrong — the center is usually near the escape boundary
   during panning, so its orbit is short and most of the screen falls back to
   BigFloat. Replaced with a single coarse `RebaseMatrix` pass to find an
   adequately deep point.
2. **Parallelize the probe.** The low-res probe was solo (~16 ms single-thread)
   while every other worker idled. Made it cooperative: all workers help evaluate
   the small grid in one pass, capped iterations (ranking only), last reporter
   builds.
3. **Temporal stability — reuse across previews.** Every fresh probe re-selects
   a reference via max-depth search, which is discontinuous: a small pan can
   shift which grid cell wins, jumping the reference center by a full cell and
   lurching the whole delta field. Fix: cache the last-built reference
   (`lastRef_`/`globalCache`) and reuse it across previews while the camera stays
   within a validity radius; only rebuild on a real settle or when the camera
   drifts too far.
4. **Decay-aware reuse.** Naive reuse let continuous zoom keep reusing a
   reference built at a shallower zoom until it became visibly inadequate.
   Tightened the reuse trigger with a zoom-factor check (~1 octave) and a
   current-view-relative pan radius, so it rebuilds *during* motion rather than
   only after stopping — the affordable cooperative probe from (2) is what makes
   this cheap enough to do continuously.

**User's standing instruction throughout this phase:** ignore the fact that the
reused cache pointer can be reallocated out from under a straggler — ownership
and reclamation ("hazard pointers or smart memory pooling") were deferred
explicitly to be handled later. (See §9 — this is where that debt came due.)

---

## 4. BLA (bilinear approximation) implementation

**Prompt:** "lets try tier 3" (of a tiered performance plan: SIMD render, cheaper
BigFloat reference, then BLA iteration-skipping).

**What was built:** a proper Zhuoran BLA merge tree replacing the old
start-of-orbit-only Taylor skip. Level 0 entries are `{A=2Zᵢ, B=1, r=tol·|2Zᵢ|}`
per reference-orbit point; higher levels merge adjacent pairs
(`A = Ay·Ax`, `B = Ay·Bx + By`, `r = min(rx, max(0, (ry − |Bx|·dc_max)/|Ax|))`),
doubling the skip length each level. The render walk greedily picks the largest
valid level at each step instead of single-stepping through the whole orbit.

---

## 5. Crash diagnosis — malloc invalid size / double free

**Prompt:** "the corruption is gone, I just am getting somewhat not so good
rendering on the low resolution frames" (after an earlier crash report of
`malloc invalid size`/`double free` when entering a perturbation job).

**Root cause:** concurrent rebuilds picking the *same* cache slot. The original
double-buffer scheme derived its write target from the single `activeCache_`
pointer; once preemptive scheduling let a settle abort an in-flight preview (and
vice versa), two rebuilds could overlap and both write into the same slot.
`buildBLA`'s `blaLevels.clear()` + nested-vector reallocation under concurrent
writers corrupted the heap.

**Fix (this iteration):** move the cache **job-local** — the frame's leader
builds the reference into its own thread-local `ReferenceCache` and publishes
the *pointer* through the job; there is no shared engine-owned cache slot to
collide on. This is the design that led to the `workerLocalCache_` array
central to §9.

---

## 6. `percentageStatus` bug — refinement counter always 0/1

**Prompt:** "I wanted to add a loading Refinement progress counter
percentageStatus but the gui doesn't use it" → later: "The progress bar doesn't
work, I don't get why, maybe the gui isn't polling the status method frequently
enough."

**Two separate bugs, found in sequence:**
1. Precedence bug in both `PerturbationJob::percentageStatus` and
   `EscapeTimeJob::percentageStatus`: `x & (~MASK) * 100` instead of
   `((x & MASK) * 100) / total` — operator precedence made the mask apply after
   the multiply.
2. **The real culprit**, found after fixing (1) didn't help: `RenderJob::percentageStatus`'s
   `std::visit` lambda declared its return type as `-> bool` instead of
   `-> unsigned int`, silently truncating every real percentage to 0 or 1 before
   it ever reached the caller. Polling frequency was never the issue — the value
   itself was being crushed at a layer above the fix in (1).

---

## 7. GUI feature batch (Phase A–C)

**Prompt (verbatim, this set the whole batch's scope):** "I want to concentrate
on the GUI, first I wanted to add a loading Refinement progress counter... I want
to make the Nav iterations and Target Iteration settable via keyboard instead of
buttons and apply, moreover I want a toggle which allows them to match... I'd
also want a set of color palettes to choose from, moreover I'd want to start
working on the export function... export a PNG... export path animation, i'd
want it to work in reverse, i zoom on a point and frames are generated in a way
that from that point they zoom back a bit to the center."

**Delivered:**
- Keyboard-editable Nav/Target iteration fields (click-to-focus, digit capture,
  Enter commits, Escape cancels) replacing stepper+Apply buttons, plus a "Match
  Nav to Target" checkbox.
- Five color-palette presets (`gui/Palettes.hpp`) with a cycle selector wired to
  `MandelbrotEngine::SetPalette`.
- PNG export and path-animation export (see §8 for the CPU/direction rework of
  the latter).

**Decisions made explicitly and confirmed by the user:** shell out to `zenity`
for the save dialog and `ffmpeg` for video encoding rather than vendoring a
dialog/encoder library — both were confirmed present on the target machine.

---

## 8. The black-screen bug and its two follow-on fixes — Zhuoran rebasing

**Prompt:** "I'm still getting some problems with perturbation, whenever the
rendering is fast then no problem, but in some unfortunate paths the rendering
goes so slow and then the image generated is full of black tiles." → "now I'm
getting weird tilings in the low resolution mode which also appear in the high
resolution mode... If the user zooms in a part which has not any structures the
screen quickly becomes black."

**Root cause:** the existing design treated "pixel outran the reference orbit"
as a *glitch to fix by finding a deeper reference* — which is unsatisfiable in a
structureless region where no point is meaningfully deeper than any other. Two
concrete bugs fell out of that assumption:
1. The glitch-resolution round picker compared `brk` values that were always
   identical (every stranded pixel exits the walk with the same "ran out of
   orbit" marker), so "pick the deepest" silently picked the first pixel in
   tile-scan order — causing the tile-boundary artifacts.
2. With no deeper reference to find, resolution rounds made no progress and the
   finalizer painted every unresolved survivor as flat interior — the black
   tiles.

**Fix — Zhuoran rebasing in the render loop itself:** when the reference orbit
is exhausted, or when `|δ|` exceeds the full point value (the standard
Pauldelbrot cancellation trigger), re-express the pixel against `Z₀ = 0` by
setting `δ ← z` and restarting the orbit index at 0. This is exact
(`z = Zₘ + δ`, `Z₀ = 0` by construction) and costs nothing, so a short reference
can track a pixel to arbitrary depth without ever stranding it. This entirely
subsumed the old post-render glitch-resolution phase.

**Immediate regression from the rebasing patch, found and fixed same session:**
"now I'm getting weird tilings... more like a collage which doesn't fit
together." The rebase branch reset the orbit index `m` to 0 but reused the
*stale* `Z_m` value (read before the reset) in the very next single-step
calculation, injecting a spurious `2·Z_old·δ` term at every rebase. Since BLA
runs were unaffected (they re-derive `Z_m` from the corrected index), only
single steps near escape were wrong — producing internally-smooth but
mutually-misaligned regions, i.e. the "collage." Fixed by zeroing the local
`zr`/`zi` alongside `m` in the rebase branch.

---

## 9. Large refactor batch — dead code, duplication, GUI polish, export UX

**Prompt:** "remove all dead or unreachable code / merge common methods in
utilities / add the progress status to also the change palette method / Fix the
progress status counter for a perturbation job to also include the cache
computation... only trigger a re-rendering only when its actually needed /
disable all mouse input when box zoom is enabled / remove the fps counter /
display text on the option panel which tells how to close it / when all panels
are closed display a little button on top right which if clicked reopens all /
Make the export frame and path animation [work]."

**Dead-code removal (three staged commits, ordered so a hang risk landed last):**
removed the entire post-render "Phase 4" glitch-resolution subsystem (made
unreachable by the rebasing fix in §8 — nothing appended to its buffer anymore,
but it still allocated ~50 MB per cache publish and ran an all-workers spin
barrier for nothing), the pre-existing dead Phase-1 validation subsystem, and a
long tail of zero-caller functions and write-only members across the codebase.
Also fixed an unreachable `notify_all()` placed after a `return`.

**Duplication merged (only where it didn't obscure hot-loop or lock-free
intent):** a shared GUI widget layer (`gui/util/Widgets.hpp`) deduplicating
panel chrome across the sidebar and legend; unified the BigFloat orbit-build
loop across three near-identical copies and discovered one of them
(`RebaseMatrix::computeAndStoreAt`, called with the *full* iteration budget on
the high-res search path) had **no abort polling at all** — a real
responsiveness bug, fixed alongside the merge; shared `ChunkTile`/screen-origin
geometry between the two engines; a shared "elect one worker to own the shared
probe matrix" helper that also surfaced a memory-ordering inconsistency (one
call site used `acq_rel`, two others used the implicit default `seq_cst` for the
same publish). Explicitly declined to merge the render-claim/glitch-steal/
round-gated-CAS job protocols — different enough in shape and ordering
reasoning that a shared abstraction would hide the details that had already
caused bugs.

**GUI correctness fixes:** removed three sources of spurious re-render dispatch
(committing Nav Iterations while already settled, the downsample stepper while
settled, and resetting the camera when already at the home view); found and
fixed a real input-leak bug where mouse-wheel zoom still fired while box-zoom
mode was active (it was only excluded from the *pan* branch, not gated
independently); removed the FPS counter; added close hints and a unified
"reopen all panels" chip with correct hit-testing (a subtlety: the collapsed
panel chip wasn't itself registered in `isMouseOverUI`, so clicking it to reopen
also triggered a pan/box-select underneath).

**`SetPalette` no longer re-dispatches internally** — it now only swaps the
gradient (still safely, aborting and waiting for the in-flight job first, since
workers hold a live reference to it) and lets the GUI request a normal frame
through the standard scheduling path, so the recolour gets the same progress
reporting and preemption as any other frame.

**Progress counter now spans both phases of a perturbation job**, not just
render: 0–30% for the reference build (a counter bumped on the same stride that
already polls for abort, so it's free), 30–100% for render.

---

## 10. Path-animation export: CPU usage, direction, auto-length, ESC-quits-app

**Prompt:** "the cpu runs at 30 percent when rendering path animations, i
suspect it's because you add one frame at a time and threads gets suspended...
i'd want the video to play in reverse (zoom in), have the GUI determine how many
frames needed to zoom from the reset position to the point, moreover when
pressing exit to cancel the application closes."

**Four fixes:**
1. **ESC quitting the whole app (real bug, unrelated to the user's own
   hypothesis):** `SetExitKey` was never called, so ESC remained raylib's
   *default* exit key, which sets a sticky close flag that every
   `while (!WindowShouldClose())` loop in the program observes — including the
   main loop. `SetExitKey(KEY_NULL)` fixed both this and a latent instance of
   the same bug in the numeric-field editor's cancel-on-Escape handling.
2. **CPU underutilization — diagnosed as NOT frame-at-a-time dispatch** (each
   frame already saturates every worker) but two idle windows *between* frames:
   single-threaded PNG encoding on the main thread while all workers sat parked,
   and the modal progress overlay redrawing every exported frame through
   `EndDrawing`'s 60fps cap, throttling a fast render to the display refresh
   rate. Fixed by streaming raw RGBA frames directly into a long-lived `ffmpeg`
   pipe (no PNG encode step at all — ffmpeg encodes concurrently in its own
   process) and throttling the overlay redraw to ~20Hz independent of frame
   count.
3. **Direction reversed to zoom-in**, with frames generated in playback order
   (`z = exp(log(target) · t)`), and the camera held at the target's coordinates
   for the whole flight rather than drifting from a canonical home framing —
   flagged explicitly as a judgment call the user could override.
4. **Frame count auto-derived from zoom depth** (`frames_per_octave` constant)
   rather than prompted, so deeper targets proportionally take longer, capped at
   a sane maximum.

---

## 11. Hazard-pointer use-after-free — the deferred lifetime debt from §3/§5

**Prompt:** "I implemented hazard pointers for the cache of Perturbation Engine
but i'm still getting use after free."

**Diagnosis (plan-mode investigation, not yet implemented):** the hazard-pointer
scheme itself — one `ThreadLocalCell` per worker with a `hazardPtr` slot, an
owned `localCache`, and a `deferDeallocBucket` retire list, following the
standard Michael SMR pattern — is sound. The bug is a single self-referential
initializer:

```cpp
ThreadLocalCell& getThreadLocalCell() {
    static thread_local size_t idx = [this]() {
        const size_t my_idx = nextWorkerLocalCache_.fetch_add(1ul, ...);
        workerLocalCache_[idx].localCache = new ReferenceCache();  // idx, not my_idx
        return idx;                                                 // idx, not my_idx
    }();
    return workerLocalCache_[idx];
}
```

`idx` is read from *inside* the lambda that initializes it, so on every thread it
observes the zero-initialized value before dynamic init completes — every
worker silently collapses onto `workerLocalCache_[0]`, sharing one hazard slot,
one retire vector, and one `localCache` object across all threads. That defeats
per-worker isolation entirely: hazard-slot writes race and clobber each other,
the shared retire `std::vector` is mutated unlocked from multiple threads, and
concurrent leaders can `build()` into the same cache object at once.

**Fix identified:** use `my_idx` (not `idx`) both inside the lambda and as its
return value. Also flagged alongside: a genuine small leak from allocating a
throwaway `ReferenceCache` before the one actually assigned to `localCache` at
two call sites, and a now-dead `TaggedReferenceCounter refCount` member left
over from an earlier (superseded) reclamation approach.

**Status at time of writing:** plan approved in principle by the user narrative
flow but not yet exited/implemented in this log's scope — see the live plan file
for the authoritative, up-to-date task state.
