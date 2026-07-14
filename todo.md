1. Fix or understand why sometimes thread hang (both ETA and PTB but if a new frame issues they later process it)
2. Refactor GUI code
3. Add palette change to GUI
4. Add double engine::queryJobStatus() which returns the percentage of rendering
5. Add consistent settings (both Panning and Refining panels must have APPLY iteration)
6. Add the export frame function via file dialog
7. Add the export animation function via file dialog. User zooms until a frame, the animation is computed as the distance from that frame (zoomed) to the central frame (reset frame). Must generate an alert which asks the user to specify the zoom factor and computes the duration of the video assuming certain fps. Then it spawns the a file dialog for the path, and spawns a progress bar for the frame rendering percentage