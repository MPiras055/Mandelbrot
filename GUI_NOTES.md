# GUI notes

How the front-end works, so it's easy to modify. Three files:

| File | Responsibility |
|------|----------------|
| `include/gui/MandelbrotGUI.hpp` | The app: window, input, and the render loop. Glue only. |
| `include/gui/Presenter.hpp` | **All** GPU/raylib display state (textures, shader, blit). |
| `include/gui/util/{SettingsSidebar,LegendPanel,UITheme}.hpp` | The two on-screen widgets + colors. |

Plus the collaborators it drives: `engine::MandelbrotEngine` (CPU frame generation),
`engine::Camera` (viewport state — the GUI owns it and does all navigation), and
`gui::RenderSettings` (iteration/upscale knobs, edited by the sidebar).

## The render loop

`GUI::Run()` is one pass per displayed frame, top to bottom:

1. **`handleInput()`** — mouse/keyboard → camera + scheduling flags.
2. **`handleResize()`** — window resize → textures + engine canvas.
3. **`harvestAndUpload()`** — if the engine finished a frame, upload it to the GPU.
4. **`scheduleRender()`** — ask the engine for the next frame if one is wanted.
5. **`presenter_->composite()`** — draw the frame (upscaled) into an offscreen target.
6. **`BeginDrawing` → `blitToScreen()` + `drawUI()`** — target → window, then widgets.

The engine is asynchronous: you *request* a frame and later *harvest* it. The GUI
never blocks on rendering (except the very first frame, so the window opens with a
picture).

## Two-resolution scheduling (the state machine)

To stay responsive, the app renders at two qualities:

- **Preview** while navigating: `renderScale = upscaleFactor` (e.g. 8 → an eighth-res
  frame, fast, blurry).
- **Full-res** once the view settles: `renderScale = 1` (sharp "refinement" pass).

State variables (all in `GUI`):

- `renderScale_` — current desired scale (`1` = full, `>1` = preview).
- `needsUpdate_` — "a (re)dispatch is wanted".
- `inFlight_` — "the engine is currently computing a frame".
- `inFlightScale_` — the scale of that in-flight frame (drives preview-preempt + the
  "Refining…" badge).
- `wasMoving_` — edge-detects the moving → settled transition, which is what triggers
  the sharp pass.

`scheduleRender()` dispatches at most one frame at a time, with one exception: a
settle may **preempt** an in-flight preview so refinement starts immediately.
`disableRefinement` (sidebar) skips the full-res pass and keeps the last preview.

## Why display used to shear / stripe (fixed)

The old GUI kept two fixed textures (`tex_hires`, `tex_lores`) and sized the low-res
one as `width / upscaleFactor` at *texture-creation* time, while the engine rendered
the frame at `width / renderScale` at *dispatch* time. Those are computed at
different moments and round independently, so a 1-pixel disagreement (e.g. the window
client area is 1276×716, not 1280×720, because of decorations → `159` vs `160`) makes
the uploaded row stride disagree with the texture width. `UpdateTexture` then reads
each row slightly offset → a diagonal **shear** at low-res and horizontal **stripes**
at full-res.

`Presenter` fixes this structurally: it keeps **one** frame texture and recreates it
to the incoming frame's *exact* `width × height` on every upload. The upload is always
tightly packed against a matching texture, so a stride mismatch is impossible. (The
engine itself was verified correct in isolation — it renders a clean set headlessly.)

## Input reference

- **Drag** — pan (when "Allow Dragging" is on).
- **Drag a box** — zoom into it (when "Allow Dragging" is off); records an undo point.
- **Wheel** — zoom toward the cursor.
- **Ctrl + Z** — undo a box-zoom (only when dragging is off).
- **R** / legend button — reset camera.
- **L** — toggle telemetry panel · **Tab** — toggle sidebar.
- **Ctrl + H** — hide all UI · **Ctrl + +/-** — scale the UI.

## Notes / gotchas

- The `Presenter` is built *after* `InitWindow` (it needs a GL context), hence
  `std::optional<Presenter>`.
- `handleResize()` clears `inFlight_`: `engine.resizeScreen()` aborts the in-flight
  frame, which will never arrive, so the scheduler must know to request a fresh one.
- Preview vs full-res is chosen purely by `renderScale_`; the engine is told the
  target render dimensions and knows nothing about "preview" vs "refine".
