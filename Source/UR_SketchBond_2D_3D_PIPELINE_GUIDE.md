# UR_SketchBond 2D Parsing and 3D Reconstruction Guide

This document describes the actual implementation of the project's capture,
2D parsing, 3D reconstruction, material inheritance, runtime Boolean, undo,
and session-reset pipelines.

It is intended as the first document an agent should read before modifying the
system. The descriptions below follow the current source behavior rather than
an abstract design.

## 1. Recommended Reading Order

Read the source in this order:

1. `Test_0529_1510/FromLZGameViewportClient.cpp`
   - Keyboard entry points and per-frame capture completion.
2. `Test_0529_1510/FromLZCaptureUtils.cpp`
   - Orthographic capture, line-art generation, face segmentation, and
     actor/material ID rendering.
3. `Test_0529_1510/FromLZSketchBoard.cpp`
   - User drawing, saving, Proceed, and Undo.
4. `Test_0529_1510/FromLZSketchProcessor.cpp`
   - Selection and compositing of the latest sketch and capture.
5. `Test_0529_1510/FromLZSketch2DProcessor.cpp`
   - High-level 2D stage orchestration and per-press output directories.
6. `Test_0529_1510/FromLZImageOps.cpp`
   - Skeleton processing, stroke graph construction, cap-loop detection,
     green-side analysis, and action generation.
7. `Test_0529_1510/FromLZFaceReconstructor.cpp`
   - Face selection, orthographic unprojection, solid construction, material
     assignment, spawning, Boolean operations, and undo.
8. `Test_0529_1510/FromLZManifoldBoolean.cpp`
   - Manifold3D conversion and Boolean difference.
9. `Test_0529_1510/FromLZSessionReset.cpp`
   - Generation cancellation, output archiving, cleanup, and level reload.

## 2. End-to-End Execution

```text
Enter
  -> switch the viewport to a stable orthographic projection
  -> capture depth and normals
  -> generate black scene contours
  -> segment planar face regions
  -> render actor/material IDs
  -> open the sketch board

User draws red, green, or blue marks
  -> Space or Proceed
  -> save the sketch
  -> composite sketch marks over the captured black line art
  -> clean and skeletonize the image
  -> trace and classify colored strokes
  -> detect red/black cap loops
  -> use green strokes to determine translation and action
  -> write Action.json and cap geometry

Action data plus capture metadata
  -> select a captured planar face
  -> unproject the 2D cap through the exact orthographic matrix
  -> construct a closed prism
  -> attach: spawn the prism
  -> excavate: subtract the prism from existing scene geometry
```

The pipeline does not infer arbitrary 3D geometry from a single image. It uses
captured planar geometry, an exact orthographic projection, and a green 2D
translation to create a prism along the selected face normal.

## 3. Input and Runtime Entry Points

`UFromLZGameViewportClient::InputKey` is the main interaction entry point.

- `Enter` calls `FFromLZCaptureUtils::BeginCaptureFromWorld`.
- `Space` saves the current sketch and processes it. If no board is active, it
  directly processes the latest saved sketch.
- `Shift` calls
  `FFromLZFaceReconstructor::RestoreStep11RuntimeBooleans`.
- The viewport client's `Tick` calls
  `FFromLZCaptureUtils::CompletePendingCapture`.
- Viewport draw notifications are used to determine when the temporary
  orthographic view is actually stable.

The workflow is asynchronous. A session-generation integer prevents results
from an old task from being applied after a reset or level reload.

## 4. Orthographic Scene Capture

The capture begins in `FFromLZCaptureUtils::BeginCaptureFromWorld`.

### 4.1 Projection setup

The implementation:

1. Saves the original perspective viewport state.
2. Temporarily overrides the camera to orthographic projection.
3. Uses an orthographic width of `1536` when no valid width is available.
4. Uses a near/far range approximately equal to `0` and `2097152`.
5. Waits roughly two seconds and requires stable orthographic viewport frames.
6. Reads the final projection matrix from the real viewport.
7. Gives that exact matrix to `SceneCapture2D` as its custom projection matrix.
8. Restores the original perspective viewport after capture.

Temporal antialiasing, motion blur, and related effects are disabled to avoid
breaking pixel-level correspondence.

The final projection matrix is essential. Later reconstruction requires valid
orthographic values for:

```text
M[0][0], M[1][1], M[3][0], M[3][1]
```

Reconstruction does not fall back to an approximate `OrthoWidth` conversion.

### 4.2 Depth and normal capture

The scene capture renders floating-point scene depth and world-space normals.
These buffers are used to generate line art and planar face regions.

### 4.3 Line-art generation

Scene line art is black on a white background.

- A depth edge is detected from relative second-order depth change.
- The depth threshold is approximately `0.0015`.
- A normal edge is detected with a Sobel-style normal gradient.
- The normal-gradient threshold is approximately `0.3`.
- A pixel becomes black when either depth or normal edge detection succeeds.

These black lines become the environmental contours used by the 2D graph.

## 5. Captured Face Segmentation

`SaveNormalFaces` segments visible geometry into approximately planar regions.

The segmentation rules are:

- Pixels near `maximumDepth * 0.999` are treated as background.
- A usable normal must have length greater than `0.1`.
- Flood fill uses 8-neighbor connectivity.
- Neighboring normals may differ by at most `12` degrees.
- Relative neighboring depth difference may not exceed `0.02`.
- Regions smaller than `200` pixels are removed.
- Remaining regions are sorted by area and assigned unique colors.

For each accepted region, the code:

1. Traces its boundary with a Moore-neighborhood contour algorithm.
2. Simplifies the boundary with Ramer-Douglas-Peucker, epsilon `4`.
3. Calculates an average plane point and average normal.
4. Unprojects representative pixels through the exact capture projection.
5. Writes the 2D and 3D key points to the face JSON.

The face-color PNG is later used as a pixel-accurate lookup table during 3D
source-face selection.

## 6. Actor and Material ID Buffer

The capture stage also builds an actor/material ID image on the CPU.

Static-mesh and procedural-mesh triangles are:

1. Transformed to world space.
2. Projected with the same capture camera and projection matrix.
3. Rasterized into an image-space Z-buffer.
4. Assigned a unique color representing actor, component, and material slot.

Only the front-most triangle survives at each pixel. A JSON file maps each
unique ID color back to its actor/component/material metadata.

This buffer is optional for geometric reconstruction but is the preferred
source for assigning the material of a newly attached solid.

## 7. Sketch Board and Source Selection

The sketch board draws RGB marks over the captured image.

- Normal brush radius: `3` pixels.
- Eraser radius: `10` pixels.
- Available drawing colors: red, green, and blue.
- Sketches are saved under `Saved/FromSketch`.

When Proceed or Space is pressed, `FFromLZSketchProcessor`:

1. Finds the newest sketch PNG.
2. Finds the newest compatible capture PNG, excluding face/debug/ID images.
3. Center-crops or pads the sketch to the capture dimensions without scaling.
4. Places every non-near-white sketch pixel over the capture.
5. Preserves captured black pixels where the sketch is white.

A sketch pixel is considered white only when its channels are above the
near-white threshold. A captured pixel is treated as black when all channels
are below approximately `128`.

The processor also writes `capture_ref.json`, recording the exact sketch,
capture, face, camera, and actor/material files consumed by that press. Agents
should follow this file when debugging mismatched inputs.

## 8. 2D Processing Stages

The high-level stage sequence is in `FromLZSketch2DProcessor.cpp`. Most
algorithms are implemented in `FromLZImageOps.cpp`.

### 8.1 Step 1: binarization and cleanup

- A pixel is background only when all RGB channels exceed `240`.
- All other pixels are foreground.
- Apply one `3x3` morphological close.
- Apply one `2x2` dilation.
- Delete 8-connected foreground components smaller than `12` pixels.

### 8.2 Step 2: skeletonization

The code applies Zhang-Suen thinning for at most `100` iterations.

After thinning, skeleton components shorter than `6` pixels are removed.

### 8.3 Stroke-color classification

Skeleton pixels sample a color neighborhood of radius `2`.

- All channels above `220`: no color.
- A channel dominating the other channels by at least `30`: red, green, or
  blue.
- Otherwise: black.

Mixed primary/black pixels are resolved in favor of the primary color:

- red plus black becomes red;
- green plus black becomes green;
- blue plus black becomes blue.

### 8.4 Step 3: endpoint repair

Skeleton connectivity deliberately avoids ambiguous diagonal connections.

- Cardinal neighbors are always considered.
- A diagonal neighbor is considered only when both intervening cardinal
  pixels are absent.
- Degree `1` indicates an endpoint.
- Crossing number at least `3` indicates a branch.

Endpoint repair:

1. Finds mutually nearest endpoint pairs within `50` pixels.
2. Connects them with a one-pixel Bresenham line.
3. Temporarily removes each connector and searches for an alternative path.
4. Removes a connector when it creates an implausibly small loop.

Red/black loop protection uses a bounding-box area threshold of approximately
`1500` square pixels. Larger red/black loops are protected.

Dangling branches are pruned up to:

```text
max(30, 3 * endpointRepairDistance) = 150 pixels
```

A branch is protected when at least half of its would-be-deleted samples are
classified as red or black.

### 8.5 Step 4: skeleton tracing

Stroke tracing has two passes:

1. Trace from endpoints and branch nodes to other graph nodes.
2. Trace remaining unvisited edges, including node-free cycles.

At a choice point, continuation maximizes the direction dot product. Paths
shorter than three points are discarded.

Color splitting then:

- infers uncolored runs from neighboring colors;
- resolves primary/primary transitions using directional proximity;
- absorbs short color runs below approximately `18` pixels of arc length;
- outputs monochromatic strokes.

### 8.6 Step 5: corner splitting

Corner detection performs PCA on left and right arc-length windows of roughly
`30` pixels.

- At least three points are required on each side.
- The unoriented direction change must be at least `25` degrees.
- Accepted peaks must be separated by at least `10` pixels of arc length.
- Detection and non-maximum suppression iterate up to five times.
- The split point is duplicated in the two resulting strokes.

### 8.7 Step 6: collinear merging

Only same-color strokes are eligible.

- Endpoint gap must be no more than `3` pixels.
- PCA/chord direction disagreement must be no more than `12` degrees.
- A merge is rejected if a third endpoint is near the proposed join.
- Candidate cost is `gap + 0.1 * angle`.
- At most `80` merges are performed.

### 8.8 Step 7: stroke metrics

Each stroke records:

- arc length;
- endpoint chord length;
- straightness;
- PCA direction;
- PCA RMS deviation;
- PCA 90th-percentile deviation;
- chord-line 90th-percentile deviation;
- length/width-style ratios.

### 8.9 Step 8: enclosed-region preview

All strokes are rasterized as barriers with thickness `3`.

Each endpoint is connected to its nearest endpoint. This particular preview
has no maximum connection-distance cutoff.

A four-neighbor flood fill starts at the image border. Background pixels not
reached by that flood are marked as enclosed.

Important: Step 8 is primarily diagnostic. Step 9 does not consume this mask;
it independently detects loops with a planar stroke graph.

## 9. Step 9: Cap and Action Analysis

Step 9 combines red user strokes, black captured contours, and green direction
strokes.

### 9.1 Red/black graph planarization

The code detects intersections between all real red/black line segments,
including collinear overlap, and splits strokes at those intersections.

Graph endpoints are snapped with a tolerance of about `5` pixels.

### 9.2 Red and black dead-end repair

Only degree-one endpoints are repaired.

1. A red endpoint first searches for a forward red segment within `20` pixels.
2. Remaining red endpoints search for a forward black segment within
   `20` pixels.
3. Black endpoints may then connect to nearby red or black endpoints.
4. Connecting to a segment interior splits that target segment.

The endpoint's outward direction is estimated over approximately five pixels
of arc length. Connections behind that direction are rejected.

### 9.3 Loop candidates

Candidate priority is:

1. Pure red loop.
2. One red component plus local black strokes.
3. Red strokes plus global black fallback strokes.

For each eligible non-black graph edge, the edge is removed and BFS searches
for another path between its endpoints. That alternative path plus the removed
edge forms a cycle.

A candidate must:

- contain at least two edges;
- contain at least one real red stroke;
- have a bounding-box area of at least `1500` square pixels when borrowing
  local or fallback black lines.

Candidates prefer:

- a valid local green chain that produces `attach` or `excavate` is mandatory;
- lower priority number;
- shorter longest synthetic-connector arc length, with no synthetic edge
  treated as zero;
- shorter total black-stroke arc length;
- larger loop area when both candidates are red-only;
- more real red strokes;
- fewer total graph edges;
- lower anchor-stroke ID.

A real red stroke can be consumed by only one selected cap loop.

Local-green discovery and action measurement therefore happen before loop
selection. A candidate is rejected before red-stroke consumption when:

- no green endpoint is local to its real red strokes;
- no non-zero green chain can be traced;
- inside/outside measurement produces neither `attach` nor `excavate`.

`09_loop_candidates.json` records the prefiltered action, local green IDs,
longest synthetic length, total black length, and rejection reason for every
candidate.

### 9.4 Local green-chain tracing

Green strokes are considered local when an endpoint is within `50` pixels of
real red cap geometry.

Green chain tracing:

- begins at endpoints;
- permits inter-stroke gaps up to `10` pixels;
- requires continuation direction change below `45` degrees;
- never revisits a stroke;
- searches both directions;
- selects the path with greatest total stroke-plus-gap length;
- breaks ties using endpoint chord length.

The chosen chain is oriented from the cap-near endpoint toward the cap-far
endpoint. Its start-to-end vector translates the cap polygon.

### 9.5 Attach versus excavate

The cap polygon is rasterized and local green length is measured inside and
outside it.

- More green length inside the cap: `excavate`.
- More green length outside the cap: `attach`.
- Equal length or no valid local green chain: no action.

The constant `InteriorGreenMinInsideLengthPx = 10` is calculated and emitted
in diagnostics, but the final action is primarily chosen by the inside/outside
length comparison.

Step 9 writes per-component files including:

```text
Action.json
09_cap_extrusion.json
09_cap_extrusion.png
graph and candidate diagnostics
```

### 9.6 Current blue-line behavior

Blue strokes pass through binarization, skeletonization, color splitting,
corner processing, merging, and metric calculation.

They are not used by Step 9 for cap detection, action selection, or 3D
reconstruction. Blue currently has no final 3D semantic meaning.

## 10. 3D Input Validation

`FFromLZFaceReconstructor::ProcessPress` loads the source reference, face data,
camera data, action data, cap data, and optional actor/material ID data.

Reconstruction requires:

- orthographic capture metadata;
- a usable exact projection matrix;
- compatible capture, face, and actor/material projection matrices.

The code compares the key matrix terms with a very strict relative tolerance,
approximately `1e-8`. Inconsistent projection data causes reconstruction to
stop rather than silently using approximate coordinates.

## 11. Source Polygon Semantics

The polygon used to select the source face depends on the action.

For `excavate`:

```text
source polygon = cap_polygon
copied polygon = cap_polygon_translated
```

For `attach`:

```text
source polygon = cap_polygon_translated
copied polygon = cap_polygon
```

The orientation is significant because the chosen source face provides the
plane and normal used for unprojection and extrusion.

## 12. Captured Face Selection

The source polygon is scaled into face-image coordinates and rasterized.

Candidate faces:

1. Must overlap the polygon mask by at least approximately five percent of
   the mask area.
2. Are identified through their unique face-image colors.
3. Receive a ray/plane intersection test from the polygon centroid.
4. Have their world normal projected into image space.
5. Are compared against all available green-side directions.

The normal comparison is unoriented:

- angle at most `30` degrees: accepted;
- angle below `10` degrees: preferred.

Among preferred candidates, the face nearest the camera is chosen. If there is
no preferred candidate, the nearest passing candidate is used.

The selected face's full key-point polygon is triangulated for reconstruction
diagnostics, but it is not itself the cap-prism mesh.

## 13. Orthographic Pixel-to-World Conversion

For pixel `(x, y)`:

1. Convert the pixel center to NDC.
2. Recover view-space right/up values:

```text
right = (ndcX - M30) / M00
up    = (ndcY - M31) / M11
```

3. Build a world-space orthographic ray from the capture camera basis.
4. Intersect that ray with the selected source plane.

This produces one source-world point for each source-loop pixel.

## 14. Cap-Loop Preparation

Before solid construction, the paired source and copied loops are cleaned:

- remove extremely close points;
- remove duplicate points;
- remove nearly collinear points with a tolerance near `0.75` pixels;
- keep the two loops paired;
- when over `384` points, apply RDP with epsilon `1.25`;
- decimate further when still above `384` points.

The average paired 2D displacement is the screen-space extrusion vector.

## 15. Face-UV Cap BBOX Regularization

After the original source loop is unprojected onto the selected face plane, the
reconstructor attempts to recognize and regularize a rectangular cap.

This stage is an enhancement: any invalid or degenerate input falls back to the
original cap and does not independently fail reconstruction.

### 15.1 Stable face UV axes

The captured face is viewed directly along its normal and treated as a 2D
plane.

1. Project the captured face boundary into a temporary face-plane basis.
2. Compute the 2D convex hull.
3. Enumerate hull-edge directions to find the minimum-area oriented BBOX.
4. Use the longer BBOX direction as `U` and the shorter direction as `V`.
5. When the two extents differ by no more than one percent, choose the axis
   whose image projection is more horizontal as `U`.
6. Flip both axes when required so `+U` projects toward image-right.
7. Ensure `U x V` points in the captured face-normal direction.

The first cap candidate uses these face-derived directions and computes its
own independent axis-aligned UV BBOX. This candidate is called the
`cap_base_bbox`. It is not expanded, clipped, or snapped to the face boundary.

### 15.2 Two-stage 70-percent rule

The first-stage fill ratio is:

```text
cap_base_bbox_ratio =
    abs(area(original cap in face UV))
    / area(cap_base_bbox)
```

The configured threshold is exactly:

```text
ratio >= 0.70
```

When `cap_base_bbox_ratio >= 0.70`, the source cap is replaced by the four
base-oriented BBOX corners and no second-stage test is needed.

Only when the base-oriented ratio fails, the code:

1. Computes the original cap's convex hull in the same face UV plane.
2. Finds the cap hull's own minimum-area oriented BBOX.
3. Calls this candidate the `cap_minimum_bbox`.
4. Calculates:

```text
cap_minimum_bbox_ratio =
    abs(area(original cap in face UV))
    / area(cap_minimum_bbox)
```

5. Uses the cap minimum BBOX when its ratio is at least `0.70`.

The final priority is:

```text
if cap_base_bbox_ratio >= 0.70:
    final_cap = cap_base_bbox
else if cap_minimum_bbox_ratio >= 0.70:
    final_cap = cap_minimum_bbox
else:
    final_cap = original_cap
```

The second BBOX remains on the selected face plane, but its orientation comes
from the cap itself and may be rotated relative to the base face.

Both stages intentionally use only the area-ratio rule. A circle can have a
ratio near `0.785` and is therefore expected to be regularized as a rectangle.
No circularity, edge-coverage, or corner test is applied.

### 15.3 Copied-loop reconstruction

Before regularization, the code preserves the original average source-to-copy
pixel displacement:

```text
original_delta_2d =
    average(originalCopiedPixel[i] - originalSourcePixel[i])
```

After a successful BBOX replacement, each new source-world corner is projected
back into the capture image and its copied target is:

```text
copiedPixel[i] =
    project(regularizedSourceWorld[i]) + original_delta_2d
```

The displacement remains in capture-image pixel space. It is not converted
into face UV, so existing green-stroke and attach/excavate direction semantics
are preserved.

### 15.4 Fallback cases

The original cap is restored when any of the following occurs:

- fewer than three usable face or cap points;
- invalid face-plane basis;
- degenerate face convex hull or oriented BBOX;
- non-finite face, cap, UV, or world values;
- degenerate cap polygon, base BBOX, cap hull, or minimum BBOX area;
- failed UV-to-world conversion;
- failed orthographic reprojection of regularized corners.

Both ratios falling below `0.70` is a normal non-application decision, not a
fallback error.

### 15.5 Debug outputs

The component's Step 10 output directory receives:

```text
10_cap_face_uv_overview.png
10_cap_face_uv_detail.png
10_cap_bbox_regularization.json
10_cap_bbox_regularization_world.json
```

Both PNGs use equal UV scaling:

- dark gray: captured face boundary;
- light gray: face convex hull;
- cyan: face minimum-area oriented BBOX;
- red: original cap;
- yellow: face-oriented `cap_base_bbox`;
- purple: cap-oriented `cap_minimum_bbox`, when the second stage was computed;
- green: final cap used by reconstruction;
- red/blue arrows: stabilized `+U/+V`.

The overview is framed around the face and cap, while the detail image is
framed around the cap. The JSON files record both BBOX candidates, both ratios,
the selected geometry (`cap_base_bbox`, `cap_minimum_bbox`, or `original`),
the axes, threshold, fallback reason, UV loops, world loops, and original pixel
displacement.

## 16. Extrusion Depth

The source face normal is oriented toward the source-to-copied screen
translation.

For world direction `n`, the pixel displacement caused by one world unit is:

```text
pixelPerWorld.x =
    dot(n, cameraRight) * 0.5 * imageWidth * M00

pixelPerWorld.y =
    -dot(n, cameraUp) * 0.5 * imageHeight * M11
```

Given average source-to-copied displacement `delta2D`:

```text
depth =
    dot(pixelPerWorld, delta2D)
    / dot(pixelPerWorld, pixelPerWorld)
```

The depth must exceed approximately `0.0001`.

Copied 3D points are:

```text
copiedWorld = sourceWorld + orientedNormal * depth
```

Every copied point is projected back into the image for validation. Allowed
reprojection error is:

```text
max(25 pixels, 0.75 * screenTranslationLength)
```

At least three valid paired points are required.

## 17. Closed Solid Construction

The source 2D loop is triangulated with ear clipping.

If ear clipping cannot find a valid ear, the implementation falls back to a
triangle fan.

The final prism contains:

- a source cap;
- a copied cap with opposite triangle winding;
- two side triangles for every loop edge.

Before triangle emission, paired loops may be reversed so the source-cap
normal faces opposite the oriented extrusion direction.

For excavation, the cutter is enlarged:

- approximately `1.2` along its primary axis;
- approximately `1.1` perpendicular to that axis.

This makes Boolean penetration more reliable at boundaries.

## 18. Attach Material Selection

For successful attach solids, the preferred material path is:

1. Rasterize the source polygon in actor/material-ID image space.
2. Optionally filter samples to the selected captured face.
3. Count valid actor/material colors.
4. Select the majority color.
5. Resolve its actor, primitive component, and material slot.

If that lookup fails, visible geometry is searched using average
point-to-triangle distance from loop samples and the loop centroid.

If no source material can be recovered, the generated mesh uses its configured
vertex-color/debug material path.

## 19. Step 11 Runtime Boolean

Excavation uses Manifold3D through `FFromLZManifoldBoolean::Difference`:

```text
result = target - cutter
```

Relevant tolerances:

- Manifold input tolerance: `0.001 cm`.
- Vertex weld tolerance: `0.01 cm`.
- Target/cutter bounds expansion: `1 cm`.
- Minimum renderable result edge: `0.05 cm`.

Before Boolean evaluation:

- meshes are converted to `FDynamicMesh3`;
- vertices within the weld tolerance are merged;
- boundary and non-manifold edges are diagnosed;
- signed volume is used to normalize orientation for Manifold.

The primary pass uses the target's render orientation. A fallback pass tries
the reversed target orientation.

A non-empty result is accepted only when:

- it is a closed two-manifold;
- it has no non-manifold edges;
- its minimum edge length is at least `0.05 cm`;
- volume reduction is at least:

```text
max(1 cubic centimeter, targetVolume * 0.0001)
```

An empty result can be valid when the cutter completely removes the target.

After a successful operation:

- the original component is hidden and tagged for the current press;
- a new procedural-mesh Boolean-result actor is created;
- accepted cutters are hidden;
- generated attach solids remain visible.

## 20. Boolean Target Selection and Ordering

Boolean targets include:

- visible non-runtime static-mesh components;
- procedural meshes created by previous attach operations;
- prior Boolean-result procedural meshes.

Debug reconstructed faces and cutter meshes are excluded as targets.

Important ordering consequence:

The current press applies its excavation cutters before spawning the current
press's attach meshes. Therefore, an attach mesh created in the same press is
not cut by that press's excavate operation. It can become a Boolean target in a
later press.

This is an inference from the current game-thread operation order and should
be rechecked if spawning or Boolean scheduling is changed.

## 21. Undo

Each Step 11 operation is tagged with a press ID.

Undo:

1. Pops the most recent active press.
2. Destroys generated attach and Boolean-result actors for that press.
3. Restores source components hidden by that press.
4. Restores accepted cutters and related runtime visibility as required.

The sketch-board Undo button and the viewport `Shift` key use the same
restoration entry point.

## 22. Session Reset and Generated Data

2D processing uses worker threads. Generation checks prevent stale tasks from
spawning geometry after a reset.

The Tab/reset sequence:

1. Increments the session generation.
2. Waits for active processing tasks.
3. Cancels pending capture.
4. Closes the sketch board.
5. Restores runtime Step 11 state.
6. Archives generated Saved folders.
7. Clears the working folders.
8. Reloads the current level.

The module's startup cleanup removes most generated/debug directories but
preserves `FromSketch`. A full Tab reset archives and clears `FromSketch` as
well.

Important generated directories include:

```text
Saved/FromLZCaptures
Saved/FromSketch
Saved/FromProcess
Saved/2DDebug
Saved/FromAction
```

## 23. Semantic Summary

Current stroke semantics are:

- Red: defines the intended cap boundary.
- Black: captured scene contours that may complete a red cap loop.
- Green: defines screen-space translation and determines attach/excavate from
  its outside/inside relationship to the cap.
- Blue: parsed and diagnosed in 2D but currently unused for 3D actions.

Current action semantics are:

- `attach`: build and spawn a closed prism using the translated-side face as
  the source plane.
- `excavate`: build an enlarged closed cutter prism and subtract it from
  existing visible targets.

## 24. Important Non-Obvious Constraints

Agents modifying this pipeline should preserve or consciously replace these
properties:

1. Capture and reconstruction must use the exact same orthographic matrix.
2. The 2D Step 8 enclosed mask is not the Step 9 cap detector.
3. Green's `10 px` interior threshold is diagnostic; inside/outside comparison
   is what chooses the action.
4. Blue has no current Step 9 or 3D meaning.
5. Real red strokes are allocated to at most one selected cap candidate.
6. Borrowed-black loops below `1500 px^2` are rejected.
7. Source/copy polygon order differs between attach and excavate.
8. Current-press excavation occurs before current-press attach spawning.
9. Face PNG, capture metadata, and material-ID metadata must describe the same
   projection.
10. Generated solids must remain closed and consistently wound for Manifold3D.
11. `cap_base_bbox` uses the face's oriented axes but the cap's own extents; it
    never expands or clips the cap to the face.
12. `cap_minimum_bbox` is computed only after the base-oriented ratio fails,
    and may rotate relative to the face BBOX.
13. Both current `0.70` tests deliberately have no circle-rejection rule.

## 25. Debugging Strategy

When a reconstruction fails, inspect data in this order:

1. `capture_ref.json`
   - Confirm the expected sketch and capture were paired.
2. Capture camera/projection JSON
   - Confirm orthographic mode and matrix availability.
3. Step 1-7 debug images/JSON
   - Confirm skeleton quality, colors, corners, and merging.
4. Step 8 output
   - Use only as a visual closure diagnostic.
5. Step 9 graph and cap candidate files
   - Confirm planarization, red/black repairs, and candidate priority.
6. `Action.json`
   - Confirm local green lengths and selected action.
7. `09_cap_extrusion.json`
   - Confirm source and translated polygons and direction.
8. Face-selection diagnostics
   - Confirm overlap and projected-normal tests.
9. Face-UV BBOX regularization diagnostics
   - Confirm stable axes, original cap, BBOX, fill ratio, and fallback reason.
10. Solid-reconstruction diagnostics
   - Confirm depth, reprojection error, triangulation, and winding.
11. Step 11 Boolean diagnostics
    - Confirm manifold validity, orientation attempts, volume change, and
      minimum-edge rejection.

Do not diagnose a late-stage Boolean failure by changing 2D thresholds until
the projection, face selection, solid closure, and mesh diagnostics have been
checked independently.
