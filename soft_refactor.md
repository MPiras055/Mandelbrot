# soft_refactor.md — Phase 1: Entities Refactor (no behaviour change)

This is Phase 1 of a two-phase refactor. Phase 1 introduces the new class entities
and behaviour-preserving extractions **only**. All runtime-behaviour changes are
Phase 2 (see bottom).

## Invariant

Every change here is **behaviour-preserving**: identical rendered pixels for
identical inputs, identical threading behaviour (the known worker-hang is left
untouched — it is a Phase-2 concern), identical GUI look and controls. This phase
only *extracts, consolidates, and introduces* class entities and reorganizes files.
Nothing in the live render/sync path changes semantics.

Verification gate for the whole phase: pick a fixed set of (center, zoom, iters)
covering float-ETA (`<1e4`), double-ETA (`1e4–1e11`), and perturbation (`≥1e11`);
capture the rendered frames before the phase, and confirm they are pixel-identical
after. Build stays green at every step.

## Target file tree (Phase-1 subset)

```
include/
  core/
    Numeric.hpp     [new]  BigFloat / ComplexDouble / escape-radius, single source
    Pixel.hpp       [new]  RGBA8, layout-compatible with raylib Color
    Kernel.hpp      [new]  shared z²+c recurrence + CalculateTotalChunks (dedup)
  engine/
    Camera.hpp      [new]  extracted camera state + math + Snapshot
    MandelbrotEngine.hpp   [slimmed] holds Camera + WorkerPool, forwards
    EscapeTimeEngine.hpp / PerturbationEngine.hpp  [Pixel + Numeric/Kernel only]
    util/FrameBuffer.hpp / ColorUtil.hpp           [Pixel instead of Color]
    job/*                                          [Numeric include only]
  concurrency/
    WorkerPool.hpp  [new]  extracted thread-lifecycle owner
  gui/
    RenderSettings.hpp [new]  iterations/upscale/refining/palette struct
    MandelbrotGUI.hpp  [dedup] use standalone widgets; own undo history
    util/UITheme.hpp / SettingsSidebar.hpp / LegendPanel.hpp  [single source]
```

## Entity-by-entity spec

### 1. `core::Numeric` — dedup the numeric aliases
- Define once: `BigFloat` (`cpp_bin_float_50`), `ComplexDouble`
  (`std::complex<double>`), `ESCAPE_R2 = 4.0`.
- Replace the duplicate definitions in `MandelbrotEngine.hpp`,
  `PerturbationEngine.hpp`, `RenderJob.hpp`, `PerturbationJob.hpp` with re-exporting
  aliases pointing at `core::` (single backing definition).
- Behaviour-preserving: same types, same math.

### 2. `core::Pixel` — decouple raylib from the compute core
- `struct Pixel { uint8_t r, g, b, a; };` — same layout/order as raylib `Color`,
  `static_assert(sizeof(Pixel)==4)`.
- Swap `Color`→`Pixel` in `FrameBuffer.hpp`, `ColorUtil.hpp`, and the two engines'
  pixel writes. Remove `<raylib.h>` from those core headers.
- The GUI uploads with `reinterpret_cast<const Color*>(pixels)` at the single
  `UpdateTexture` call — bit-identical, so behaviour-preserving.
- Convert `global_gradient_` stops to `Pixel` literals (same RGBA values).

### 3. `core::Kernel` — dedup the recurrence + chunk count (no semantic change)
- Provide the shared scalar `z²+c` step helper and **one** `CalculateTotalChunks(w,h)`
  = the existing `ceil(w/32)·ceil(h/32)` formula, called by both engines.
- **Important:** keep the *current* semantics exactly — ETA still consumes it as
  row-bands, PTB as 2D tiles. The zero-height-band *fix* is Phase 2.

### 4. `engine::Camera` — extract camera state + math (pure move)
- Move out of `MandelbrotEngine`: `offset{X,Y}` (`BigFloat`), `zoom`, the damped
  `target*` twins, and `pan / applyZoom / updateCamera / warpCamera /
  calculateBoxSnapshot / getCurrentSnapshot / resetCamera`, plus the `CameraSnapshot`
  struct (→ `Camera::Snapshot`).
- `MandelbrotEngine` holds a `Camera cam;` and forwards its existing public methods.
- `updateCamera(float dt)` takes `dt` as a parameter; the GUI passes
  `GetFrameTime()` — numerically identical, but the raylib clock leaves the core.
- **Undo history stays in the GUI** (user decision): the GUI keeps its
  `std::vector<Camera::Snapshot> historyStack`; only the type's home moved.

### 5. `concurrency::WorkerPool` — extract thread lifecycle (pure move)
- Owns `std::vector<std::thread>`, the pool-size calc (`hw_concurrency-1`, min 2),
  `stopPool`, and join-on-destroy. Constructed with the worker routine callable.
- **The worker routine body is unchanged** — still uses `acquire/release/
  releaseWait` and `TaggedReferenceCounter`; the hang stays. Phase 2 rewrites it.

### 6. `gui::RenderSettings` + widget dedup
- `struct RenderSettings { iterations; upscaleFactor; refiningIters;
  disableRefinement; palette; };` owned by the app. `SettingsSidebar` reads/writes
  it; dispatch reads it. Behaviour-preserving field relocation.
- Delete the **inline** copies of `UITheme`/`SettingsSidebar`/`LegendPanel` from
  `MandelbrotGUI.hpp`; include the standalone `gui/util/*` files. Reconcile their
  drift to *current live* behaviour (legend PTB badge calls
  `getPerturbationThreshold()`, not the stale `1e12`).
- The god-class split and the actual UI rewrite (raygui, new look) are **Phase 2**.

### 7. Inert deletions (provably no behaviour effect)
- `include/engine/util/FrameRequest.hpp` (empty), `include/EngineInclude.zip`,
  `./a.out`, the 0-byte `src/core/*.cpp` / `src/renderer/*` / `src/ui/*.cpp` stubs.
- Keep (touched in Phase 2, not inert now): `raygui.h`, `test_perturbation.cpp`, the
  `puts("PROBING")` line, dead `ColorUtil` methods.
- Fix namespace-comment drift (`// namespace mandelbrot_engine` → `engine`/`gui`).

## Commit order (each compiles & renders identically)
1. `core::Numeric` (dedup aliases).
2. `core::Kernel` (dedup recurrence + chunk count).
3. `core::Pixel` + raylib decouple in FrameBuffer/ColorUtil/engines + GUI cast.
4. `engine::Camera` extraction + facade forwarders + `dt` param.
5. `concurrency::WorkerPool` extraction.
6. `gui::RenderSettings` + widget dedup + drift reconcile.
7. Inert deletions + namespace-comment fixes.

## Verification
- Build after each commit: `cmake -B build && cmake --build build`.
- Render-parity gate (float-ETA / double-ETA / perturbation frames) before vs after.
- App still launches, pans, zooms, box-zooms with undo, changes iterations/upscale,
  shows the legend, applies the diffusion shader — exactly as before.

---

## Deferred to Phase 2 (behavioural — NOT in this phase)
- `DoubleCanvas` + relaxed atomic-selector swap + drop max-preallocation.
- Remove `TaggedReferenceCounter`; render-chunk-ticket lifetime; **fix the hang**;
  worker-routine rewrite; `abort_latest` `% size_` fix; `create()` arg-swap fix.
- PerturbationEngine hard rewrite: engine-owned persistent **BLA cache**, 4-phase job
  (`PerturbationJobExample.hpp`), 1/16 subsample glitch validation, iterative 9×9
  neighbourhood-search rebase with rotating aggregator, **SIMD render loop**.
- ETA tiling fix (kill zero-height bands) + raise/expose the ETA→PTB threshold.
- GUI rewrite: component split, raygui adoption, monitor probing → `platform` layer,
  remove engine `InitWindow`.
