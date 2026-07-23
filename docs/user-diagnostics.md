# User Diagnostics & Contributions

A curated list of prompts where the user correctly diagnosed a bug, correctly
characterized an execution-flow problem, or made a direct technical
contribution to the design — as opposed to prompts that were feature requests,
option selections, or symptom reports resolved by later investigation.

Quotes marked *(from session summary)* are carried over from an earlier
context-compaction summary rather than the raw transcript, and may be
lightly paraphrased there.

---

## Correct bug / execution-flow diagnoses

### "Actually all threads but one pass the OUTTA JOB FLAG"
*(from session summary)*

The single most load-bearing diagnostic observation in the whole stall
investigation. A generic "it hangs" report would have pointed at the
cooperative rebase barrier. This precise observation — exactly one straggler,
everyone else reaching the exit flag — ruled that out immediately (a barrier
bug stalls all-or-nothing, never exactly one) and correctly pointed at
per-pixel work-stealing with one heavy tile: the per-pixel BigFloat fallback
storm on a short reference.

### "ok the low resolutioon frames are still messy. I'm not saying they're incorrect just when panning a little bit the image may change so much"
*(from session summary)*

Correctly separated two different failure classes that are easy to conflate:
*wrong* output vs. *unstable* output. This reframing is what turned the
investigation away from "fix the reference selection" and toward "the
reference changes discontinuously frame-to-frame" — leading directly to the
reuse/temporal-stability fix.

### "Ok but in this case I can just zoom and pan continuously for a bit until the cache actually decays and I have to stop, render the full frame in order to continue"
*(from session summary)*

A precise, correct characterization of the reuse cache's failure mode under
*continuous* motion (as opposed to a single pan) — that reuse was valid at any
single instant but decayed unboundedly over a sustained gesture with no
mechanism to refresh mid-motion. This is exactly the gap the zoom-factor +
current-view-relative radius fix closed.

### "If the user zooms in a part which has not any structures the screen quickly becomes black, still a problem of the perturbationEngine"

Correctly identified that the black-screen bug was specific to **structureless
regions**, not a general regression. This was the decisive clue: it ruled out
a broad correctness bug and pointed straight at the real root cause — the
existing design assumed a strandeded pixel could always be rescued by finding
a *deeper* reference, which is mathematically impossible when no point in the
region is meaningfully deeper than any other.

### "no actually now I'm getting weird tilings in the low resolution mode which also appear in the high resolution mode"

Correctly noted the tiling artifact was present in **both** resolution paths,
which ruled out a low-res-only cause (e.g. the cheap probe) and pointed at
something shared by both — the tile-scan-order bug in the stranded-pixel
picker.

### "Ok now the black screen doesn't appear anymore but when doing perturbation on frames that used to be fine before i get images that are kind of curled up, no actual visual artifacts more like a collage which doesn't fit together"

An unusually precise bug characterization. "Collage which doesn't fit
together" — internally coherent patches that don't align with their
neighbors — is a specific enough description that it directly suggested the
correct mechanism (a systematic per-region offset, not random noise) before
any code was read. That framing led straight to the stale-`Z` rebase bug: BLA
runs were self-consistent within a region, only the single-step path near
each rebase boundary was wrong.

### "I'm still getting some problems with perturbation, whenever the rendering is fast then no problem, but in some unfortunate paths the rendering goes so slow and then the image generated is full of black tiles"

Correctly correlated two symptoms that turned out to share one root cause —
slow rendering and black tiles both traced back to the same "no deeper
reference exists" failure mode in the pre-rebasing design (bounded-round
resolution giving up and painting flat interior).

### "when pressing exit to cancel the application closes"

A precise, correct, and completely unrelated bug report surfaced as a side
note inside a feature request. Confirmed as a real defect: `SetExitKey` was
never called, so ESC remained raylib's default exit key application-wide, not
just inside the export cancel flow the user was actually asking about.

### "i suspect it's because you add one frame at the time and threads gets suspended" (CPU at ~30% during path export)

Partially correct, and correct in the part that mattered: CPU was genuinely
underutilized and threads genuinely *were* sitting idle between renders. The
specific mechanism named (frame-at-a-time dispatch itself) wasn't quite it —
each frame already saturates every worker — but the underlying instinct
("something between renders is idling the cores") was exactly right and
pointed the investigation at the two real causes: single-threaded PNG
encoding on the main thread, and the progress overlay's 60fps redraw cap
throttling export throughput.

---

## Direct design / implementation contributions

### "Let's discard for now the fact that the cache can be reallocated so the thread which publishes it should take in account all the other threads which might reference it, ill figure it out later either via Hazard Pointers or smart memory pooling, just ignore it."
*(from session summary)*

Not a bug report but a correct **prediction**: the user identified the exact
lifetime hazard in the job-local cache design (a straggler thread reading a
pointer another thread may reclaim) before it manifested as a crash, and
correctly named both viable solution classes (hazard pointers / pooling) months
before the hazard-pointer implementation and its subsequent UAF bug actually
arrived. When the UAF did show up later, it was a bug *within* the solution
the user had already correctly scoped, not a surprise class of defect.

### "I already implemented the Cooperative leader model as you can see in PerturbationJob.hpp"
*(from session summary)*

The cooperative work-stealing leader/follower model for the rebase phase
(`claimRebase`/`reportRebase`/`advanceRebase`/`waitRebaseIteration`) was the
user's own design and implementation, reviewed and hardened rather than
authored from scratch.

### "I completely revised PerturbationEngine.hpp, PerturbationJob.hpp and I added the RebaseMatrix.hpp"
*(from session summary)*

The reference-orbit probe/search structure (`RebaseMatrix.hpp`) that
everything downstream — low-res reuse, decay-aware rebuild, the high-res
neighbourhood search — was built on top of was the user's own addition.

### "I implemented hazard pointers for the cache of Perturbation Engine"

The hazard-pointer scheme itself (`ThreadLocalCell`, per-worker `hazardPtr`
slot, `deferDeallocBucket` retire list, `protectCache`/`sweepBucket`) was the
user's own implementation of the reclamation strategy predicted correctly
months earlier (see above). The subsequent bug diagnosis found the scheme's
*design* to be sound — the defect was a single self-referential variable read
in a `static thread_local` initializer, not a flaw in the reclamation
algorithm the user designed.
