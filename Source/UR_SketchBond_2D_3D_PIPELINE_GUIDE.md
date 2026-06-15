# UR_SketchBond Pipeline Guide

This guide documents the code as it exists in this repository. It is the
authoritative map of runtime control flow, capture data, 2D parsing, cap
selection, face validation, 3D reconstruction, octilinear regularization,
material inheritance, Manifold3D Boolean operations, undo, and reset behavior.

The guide intentionally distinguishes active code from stale comments and
unused helpers. When a comment disagrees with an executed call site, the
executed call site described here wins.

## 1. Project Scope and Build

The project is an Unreal Engine 5.7 runtime module named `Test_0529_1510`.

Important project configuration:

- Default map: `/Game/Test1`.
- Default game mode: `AFromLZGameMode`.
- Game viewport client: `UFromLZGameViewportClient`.
- Default pawn: `AFromLZCameraPawn`.
- Rendering target: desktop, DX12/SM6 on Windows.
- Enabled runtime plugin: `ProceduralMeshComponent`.
- The module depends on Core, Engine, InputCore, EnhancedInput, JSON,
  ImageWrapper, RenderCore, ProceduralMeshComponent, GeometryCore, Slate, and
  SlateCore.
- Manifold3D headers are taken from `ThirdParty/Manifold/manifold`.
- Manifold is compiled into the UE module through the small translation units
  in `Source/Test_0529_1510/ManifoldBuild`.
- Build definitions disable Manifold cross sections, iostream, filesystem, and
  Tracy support. `MANIFOLD_PAR=-1` leaves Manifold's parallel mode selected by
  its own build configuration.

Binary assets under `Content/` are scene/material/mesh data, not C++ control
logic. The runtime code operates on visible static and procedural mesh
components in the loaded level.

## 2. Source Ownership and Reading Order

Read custom source in this order:

1. `Test_0529_1510.cpp`
   - Module startup cleanup and global reset input initialization.
2. `FromLZGameMode.*`, `FromLZCameraPawn.*`, `DefaultInput.ini`
   - Pawn creation and camera navigation.
3. `FromLZGameViewportClient.*`
   - Runtime keyboard entry points and capture ticking.
4. `FromLZCaptureUtils.*`
   - Offscreen orthographic camera capture, line-art capture, face
     segmentation, and actor/material ID rasterization.
5. `FromLZSketchBoard.*`
   - Slate drawing board, save/proceed, minimize, close, and undo controls.
6. `FromLZSketchProcessor.*`
   - Latest-file selection, sketch/capture alignment, compositing, and channel
     exports.
7. `FromLZSketch2DProcessor.*`
   - Per-press orchestration, async execution, and Steps 1 through 10 dispatch.
8. `FromLZImageOps.*`
   - Skeleton graph processing, color splitting, Step 9 cap/action recovery,
     and Step 9 output serialization.
9. `FromLZFaceReconstructor.*`
   - Step 9 face prevalidation, Step 10 face/solid reconstruction, world
     octilinear regularization, spawning, Step 11 Booleans, material recovery,
     undo, and OBJ/debug export.
10. `FromLZManifoldBoolean.*`
    - DynamicMesh/Manifold conversion and Boolean difference.
11. `FromLZSessionReset.*`, `FromLZPressNaming.*`
    - Session generations, Tab reset, archive behavior, and press numbering.

The vendored `ThirdParty/Manifold/manifold` tree is upstream Manifold3D
implementation code. Project-specific Boolean policy lives in
`FromLZManifoldBoolean.cpp` and `FromLZFaceReconstructor.cpp`.

## 3. Runtime Controls

`UFromLZGameViewportClient::InputKey` owns the main workflow:

| Input | Behavior |
| --- | --- |
| `Enter` | Start an orthographic scene capture. |
| `1` | Capture with the unique actor tagged `FromLZCaptureCamera1`. |
| `2` | Capture with the unique actor tagged `FromLZCaptureCamera2`. |
| `Space` | If the board exists, save it and process; otherwise process the latest saved sketch. |
| `B` | Restore a minimized sketch board. |
| Left or Right `Shift` | Undo the newest active Step 11 press. |
| `Tab` | Global Slate preprocessor starts a full session reset. |

While reset is pending, viewport input is consumed and ignored.

The viewport client also:

- calls `FFromLZSessionReset::Tick` each frame;
- calls `FFromLZCaptureUtils::CompletePendingCapture` each frame;
- reports every rendered viewport frame through
  `FFromLZCaptureUtils::NotifyViewportDrawn`.

### 3.1 Camera pawn

`AFromLZCameraPawn` has a scene root, a spring arm named `CameraBoom`, and a
camera component named `FromLZ`.

Defaults:

- spring-arm length: `1200`;
- spring-arm rotation: pitch `-45`, yaw `0`, roll `0`;
- movement speed: `1200 units/s`;
- wheel zoom step: `120`;
- zoom range: `[300, 2400]`;
- pitch range: `[-89, 89]`;
- automatic possession: player 0.

Input mapping:

- `W/S`: planar forward/back;
- `A/D`: planar left/right;
- `Q/E`: world up/down;
- mouse wheel: spring-arm zoom;
- hold left mouse: enable mouse pitch/yaw rotation.

Movement uses the current world delta time. Forward and right directions are
the camera-boom directions projected onto the XY plane.

## 4. Module Startup and Working Directories

At module startup, the following directories are deleted and recreated:

```text
Saved/2DDebug
Saved/FromAction
Saved/FromLZCaptures
Saved/FromProcess
Saved/Logs
```

`Saved/FromSketch` is only created if missing; its contents survive ordinary
module startup.

The module registers:

- a global Slate Tab-key input preprocessor;
- a post-map-load callback that clears reset state.

## 5. End-to-End Control Flow

```text
Enter
  -> resolve the controlled pawn's FromLZ camera
  -> snapshot its transform, AspectRatio, and OrthoWidth
  -> fit the camera aspect ratio inside the current game viewport size
  -> build one offscreen orthographic projection matrix
  -> capture line art, planar faces, and actor/material IDs
  -> finalize projection diagnostics
  -> open the sketch board

1 or 2
  -> find exactly one actor with the corresponding capture-camera tag
  -> require exactly one registered active camera component on that actor
  -> snapshot that camera's transform, AspectRatio, and OrthoWidth
  -> run the same offscreen orthographic capture/output pipeline as Enter
  -> open the sketch board for that exact capture

Proceed or Space
  -> save composite sketch image
  -> select latest sketch and latest main capture
  -> center crop/pad sketch to capture size
  -> composite user marks over capture line art
  -> start a worker-thread press
  -> Steps 1-8: binary/skeleton/stroke processing
  -> Step 9: planar cap graph, green action, 3D face prevalidation
  -> Step 10: repeat face validation, reconstruct and regularize solids
  -> game thread: apply excavations, then spawn cutters/attachments

Shift or board Undo
  -> pop latest active press
  -> destroy actors generated by that press
  -> restore components hidden by that press

Tab
  -> advance session generation
  -> cancel capture and close board
  -> wait for worker tasks
  -> restore all runtime geometry
  -> archive working Saved folders
  -> clear working folders
  -> reload current level
```

## 6. Offscreen Orthographic Capture

Enter, `1`, and `2` share one capture implementation. The selected source
camera and displayed player view remain unchanged. Capture is queued and
completed on the next viewport tick, but it does not wait for a rendered
orthographic viewport frame.

### 6.1 Begin

`BeginCaptureFromWorld`:

1. Rejects null world/viewport.
2. Rejects a second pending capture.
3. Finds a player controller whose pawn has a `FromLZ` camera.
4. Creates timestamped output paths under `Saved/FromLZCaptures`.
5. Saves the unchanged displayed viewport backbuffer as a reference image.
6. Snapshots the selected camera component transform and projection fields.
7. Builds an immutable orthographic capture description without changing the
   camera component or player view target.

`Enter` always resolves the `FromLZ` camera component on the controlled pawn.
It does not search tagged scene cameras.

`BeginCaptureFromTaggedCamera` is used by `1` and `2`. It:

1. resolves exactly one actor with the requested Actor Tag;
2. requires exactly one registered active camera component on that actor;
3. snapshots that camera directly;
4. enters the same offscreen capture path as Enter.

Actor Tags, not actor names or editor labels, are the packaged-build contract.
Missing or duplicate tags abort capture without falling back to `FromLZ`.

### 6.2 Dimensions and projection

The current game viewport supplies only the maximum pixel bounds. The selected
camera supplies the framing parameters:

- transform: selected camera component transform;
- aspect ratio: selected camera `AspectRatio`;
- orthographic width: selected camera `OrthoWidth`, or `1536` only when it is
  invalid;
- near plane: `0`;
- far plane: `2097152`.

Capture dimensions are the largest integer-sized rectangle derived from the
camera aspect ratio that fits inside the current viewport. For example, a
`4:3` camera in a `1920x1080` viewport captures at `1440x1080`. The image is
not stretched, letterboxed, or padded.

The code constructs a local `FMinimalViewInfo`, forces only that local
description to orthographic, and calls
`FMinimalViewInfo::CalculateProjectionMatrix`. That exact matrix and immutable
camera transform are reused by:

- final-color and debug SceneCapture2D passes;
- depth/normal line-art capture;
- planar-face segmentation and 2D-to-world unprojection;
- actor/component/material ID rasterization;
- capture JSON consumed by Steps 9 and 10.

`SceneCapture2D.CustomProjectionMatrix` receives this shared matrix.

The matrix is considered usable when:

- `M00` and `M11` are finite and non-zero;
- `M23` is approximately zero;
- `M33` is approximately one.

There is no Step 10 fallback from a missing matrix to an `OrthoWidth`
approximation.

### 6.3 Viewport and camera invariants

The capture path never calls `SetViewTarget`, never changes the source camera
projection mode or dimensions, and has no orthographic settle delay. The
player therefore keeps the same displayed view before, during, and after
Enter/`1`/`2`.

`NotifyViewportDrawn` remains as a compatibility entry point but performs no
capture-state work. Cancel or Tab reset simply drops a pending immutable
capture request; no camera or view-target restoration is required.

## 7. Capture Subject Filtering

SceneCapture uses a show-only actor list.

Eligible actors must:

- not be the controlled pawn;
- not be actor-hidden;
- contain a registered, visible static mesh with a mesh, or a registered,
  visible procedural-mesh section;
- and satisfy one of:
  - actor name/label starts with `Cube`;
  - actor name/label contains `Plane`;
  - actor is an active Step 11 attachment or Boolean result.

Excavation cutter actors are never active capture subjects.

An additional camera-relative bounding-box rule excludes an actor only when
its entire AABB is above the capture camera and the camera lies inside that
AABB's XY footprint. Actors above the camera but outside that XY footprint are
kept.

The selected actor counts and filtering mode are written into capture
metadata.

## 8. Capture Outputs

For stem `FromLZ_YYYYMMDD_HHMMSS`, the capture directory contains:

```text
FromLZ_<timestamp>.png
FromLZ_<timestamp>.json
FromLZ_<timestamp>_faces.png
FromLZ_<timestamp>_faces.json
FromLZ_<timestamp>_faces_debug.png
FromLZ_<timestamp>_actor_material_id.png
FromLZ_<timestamp>_actor_material_id.json
FromLZ_<timestamp>_debug_viewport_perspective.png
FromLZ_<timestamp>_debug_viewport_orthographic.png
FromLZ_<timestamp>_debug_fromlz_camera.png
FromLZ_<timestamp>_debug_transforms.json
FromLZ_<timestamp>_debug_depth.png
FromLZ_<timestamp>_debug_normal.png
FromLZ_<timestamp>_debug_depth_lap.png
FromLZ_<timestamp>_debug_normal_grad.png
```

The main JSON includes the selected camera transform, original/capture
projection properties, maximum viewport size, fitted capture dimensions,
camera aspect ratio, exact matrix, subject mode, and debug paths.

`_debug_viewport_perspective.png` is the unchanged displayed game viewport.
The legacy-named `_debug_viewport_orthographic.png` is an offscreen render from
the selected camera; it is not a screenshot of a switched player viewport.

## 9. Line-Art Generation

Two float render targets are captured with the same show-only actor set:

- `SCS_SceneDepth`;
- `SCS_Normal`.

Capture settings disable temporal AA, motion blur, screen percentage
distortion, and persistent render state so pixel correspondence does not
depend on temporal history.

For each pixel:

- depth edge strength is the largest second-order depth deviation in X, Y, and
  both diagonals divided by `max(depth, 1)`;
- depth threshold: `0.0015`;
- normal edge strength is the combined Sobel X/Y gradient magnitude;
- normal threshold: `0.3`.

A pixel is black if either threshold passes; otherwise it is white.

## 10. Planar Face Segmentation

`SaveNormalFaces` uses the second depth/normal pass.

Rules:

- background depth: `depth >= maxDepth * 0.999` or `depth <= 1`;
- usable normal length: greater than `0.1`;
- connectivity: 8-neighbor;
- normal continuity: at most `12 degrees`;
- relative neighbor depth difference: at most `0.02`;
- minimum region area: `200 pixels`;
- contour simplification: RDP epsilon `4 pixels`.

Accepted regions are sorted by area and assigned unique RGB IDs. The first
100 IDs use a fixed palette; additional IDs use deterministic fallback
colors with collision checks.

For each face the code:

1. traces a Moore-neighborhood contour;
2. rotates the closed contour to a stable start;
3. simplifies it;
4. averages valid depth-derived world points and normals;
5. calculates a representative 2D centroid;
6. intersects contour pixel rays with the averaged plane;
7. writes plane point, normal, 2D key points, 3D key points, ID color, area,
   and projection scale metadata.

`_faces.png` is a pixel ID lookup used by both Step 9 and Step 10.

## 11. Actor/Component/Material ID Buffer

This image is rasterized on the CPU with the exact capture transform and
projection matrix.

Supported sources:

- static mesh LOD 0 sections;
- visible procedural-mesh sections.

Every actor/component/material-slot tuple receives an integer ID encoded as a
unique RGB color. Each world-space triangle is projected and rasterized with
barycentric interpolation into a depth buffer. Only the nearest triangle at a
pixel survives.

The JSON maps color IDs to:

- actor name/path;
- component name/path/type;
- material slot;
- material name/path;
- projection metadata and subject mode.

This buffer is optional for geometry but preferred for attachment material
inheritance. If it loads successfully, its projection must exactly match the
capture and face projection.

## 12. Sketch Board

The board is a Slate overlay at viewport Z-order `200`.

Behavior:

- capture image is loaded into a transient BGRA texture;
- the drawing layer begins fully white;
- the image is aspect-fitted without changing image coordinates;
- mouse coordinates are converted back to source pixels;
- line interpolation uses integer steps between the previous/current pixel;
- red, green, blue, eraser, clear, proceed, undo, minimize, and close controls
  are available.

Actual brush radii:

- red/green/blue: `1 pixel`;
- eraser: `10 pixels`.

The board saves the full background-plus-drawing composite, not a transparent
marks-only image, as:

```text
Saved/FromSketch/Sketch_##.png
```

The number is the next `Press_##` index found in `Saved/2DDebug`.

Minimize removes only the widget and restores game-only input. `B` re-adds
the same board state. Close destroys the board state. Proceed saves, closes,
then invokes sketch processing.

## 13. Sketch and Capture Pairing

`FFromLZSketchProcessor::ProcessLatestSketch`:

1. Selects the newest PNG in `Saved/FromSketch`.
2. Selects the newest main capture PNG in `Saved/FromLZCaptures`.
3. A main capture filename must exactly match:
   `FromLZ_YYYYMMDD_HHMMSS.png`.
4. Auxiliary face/debug/ID PNGs therefore cannot be selected.
5. Ties in modification time are resolved by filename.

Proceed from an open sketch board does not perform this latest-capture lookup.
The board retains the exact main capture PNG used as its background and passes
that path together with the saved sketch to `ProcessSketch`. The resulting
`capture_ref.json` therefore remains bound to the same capture stem even if a
newer capture appears while the board is open. Space without a board retains
the latest-sketch/latest-capture behavior.

If capture dimensions differ, the sketch is center-cropped or white-padded to
the capture size. No scaling is performed.

Compositing rules:

- a sketch pixel is a mark unless all RGB channels are greater than `240`;
- sketch marks overwrite capture pixels;
- where the sketch is white, a capture pixel becomes black only when all
  capture RGB channels are below `128`;
- all remaining pixels are white.

Outputs under `Saved/FromProcess`:

```text
Sketch_fitted.png
wContextSketch_raw.png
Sketch_R.png
Sketch_G.png
Sketch_B.png
```

Channel images contain only exact `(255,0,0)`, `(0,255,0)`, or `(0,0,255)`
pixels with transparent black elsewhere. They are auxiliary outputs; the
active 2D pipeline consumes the composite buffer directly.

## 14. Press Directories and Async Lifetime

Each processing run receives:

```text
Saved/2DDebug/Press_##
Saved/FromAction/Press_##
```

Press numbering scans existing `Press_*` directories and uses max + 1.

`capture_ref.json` records the exact sketch, capture, face PNG/JSON, and
actor/material PNG/JSON paths consumed by the press.

The heavy pipeline runs on the UE thread pool. Before dispatch:

- the RGBA array is moved into the worker;
- the current session-generation integer is captured;
- active composite-task count is incremented.

Generation is checked:

- before 2D processing;
- before Step 10/11 reconstruction;
- before game-thread runtime spawning.

The task count is decremented after worker completion. Tab reset waits for it
to reach zero before deleting working data or reloading the map.

## 15. Active 2D Stage Parameters

The following values come from executed calls in
`FromLZSketch2DProcessor.cpp`.

### 15.1 Step 1: binary cleanup

Active sequence:

1. `BinarizeNonWhite(..., WhiteThreshold=240)`.
2. `RemoveSmallComponents(..., MinArea=12)`.

Foreground means any pixel whose RGB channels are not all greater than `240`.
Components are 8-connected.

Important: `MorphClose` and `Dilate2x2` helpers exist, but the current
orchestrator does not call them. The active Step 1 performs no close or
dilation.

Outputs:

```text
00_input.png
01_binary.png
```

### 15.2 Step 2: thinning

- Zhang-Suen maximum iterations: `100`;
- remove skeleton components smaller than `6 pixels`.

Output: `02_skeleton.png`.

### 15.3 Pixel color classification

Per-pixel classes:

- all channels greater than `220`: `none`;
- one channel exceeds both others by at least `30`: red, green, or blue;
- otherwise: black.

The stroke color sample window radius is `2`.

### 15.4 Step 3: endpoint cleanup

Executed parameters:

- endpoint gap tolerance: `20 pixels`;
- connector thickness: `1`;
- endpoint trend trace arc: approximately `5 pixels`;
- strict source/target endpoint angle: at most `60 degrees`;
- relaxed target endpoint angle: at most `100 degrees`;
- small-loop bounding-box threshold: `500 px²`;
- branch-prune max: automatic, `max(30, 3 * gapTol) = 60 pixels`;
- color sample radius: `2`.

Skeleton neighborhood policy:

- cardinal neighbors are always valid;
- a diagonal neighbor is valid only when both intervening cardinal pixels are
  absent;
- degree 1 is an endpoint;
- crossing number at least 3 is a branch.

#### 15.4.1 Endpoint trend

All Step 3 endpoint angle tests use the same trend calculation:

1. start at the endpoint;
2. follow its incident skeleton edge inward;
3. at a choice, continue along the neighbor that maximizes the dot product
   with the previous step direction;
4. stop after about `5 pixels` of Euclidean arc, or earlier at an endpoint or
   branch;
5. calculate `normalize(endpoint - inwardTraceEnd)`.

The traced incident-edge pixels are retained. Segment searches exclude
segments touching this set, preventing an endpoint from reconnecting
backward onto its own first approximately `5 pixels`.

#### 15.4.2 First pass: full skeleton

Input is exactly `02_skeleton.png`. Endpoints and trends are detected once
from this fixed full-skeleton snapshot.

Search order:

1. `strict_endpoint_to_endpoint`
   - distance at most `20 pixels`;
   - source and target trends must each be within `60 degrees`;
   - each endpoint records its nearest legal endpoint;
   - the pair is accepted only when the relationship is mutual;
   - acceptance consumes both endpoints.
2. `strict_endpoint_to_segment`
   - only unused endpoints initiate;
   - the target is the nearest projection on an original skeleton segment;
   - the clamped projection may coincide with a segment endpoint;
   - source trend must be within `60 degrees`;
   - the target has no direction test;
   - nearest legal projection wins and consumes the source endpoint.
3. `relaxed_endpoint_to_endpoint`
   - only unused endpoints participate;
   - source trend must be within `60 degrees`;
   - target trend may deviate by at most `100 degrees`;
   - nearest legal target in stable endpoint scan order wins;
   - acceptance consumes both endpoints.

Every connector is a one-pixel Bresenham line. `ConnectThickness` remains in
the API but does not thicken the line.

Connector color follows the source endpoint's approximately `5-pixel`
incident edge, sampled against the original color map. It can therefore be
black, red, green, or blue; unresolved color falls back to black.

The result of this pass is `03a_skeleton_connected.png`.

#### 15.4.3 Red/black extraction and second reconnect

The red/black graph is extracted from the first-pass full result:

- original skeleton pixels use sampled source color;
- synthetic pixels use their first-pass connector color;
- only red and black pixels enter the search graph;
- green and blue connectors remain in the full graph but do not enter the
  red/black search graph.

`03a_red_black_connectors.png` is this pre-reconnect graph.

Only red degree-1 endpoints initiate the second reconnect. Endpoint trends
again use the approximately `5-pixel` incident-edge trace. All candidates use
one fixed pre-reconnect red/black snapshot, so newly generated second-pass
connectors cannot become targets in the same search.

Search order:

1. `red_reconnect_strict_endpoint_to_endpoint`
   - distance at most `20 pixels`;
   - source angle at most `60 degrees`;
   - target angle at most `60 degrees`;
   - all legal pairs are sorted by distance and accepted nearest-first;
   - mutual-nearest is not required;
   - acceptance consumes both endpoints.
2. `red_reconnect_strict_endpoint_to_segment`
   - only still-unused red endpoints initiate;
   - target may be an existing branch/topology node or strict segment
     interior;
   - source angle at most `60 degrees`;
   - target has no direction test;
   - nearest node/interior projection wins and consumes the source.
3. `red_reconnect_relaxed_endpoint_to_endpoint`
   - only still-unused endpoints participate;
   - source angle at most `60 degrees`;
   - target angle at most `100 degrees`;
   - all legal pairs are sorted by distance and accepted nearest-first;
   - acceptance consumes both endpoints.

All second-pass connectors are red. They are added to both the red/black graph
and the full skeleton. The outputs are:

```text
03a_red_black_reconnected.png
03d_skeleton_reconnected.png
```

#### 15.4.4 Connector pruning and final cleanup

Small-loop pruning starts from `03d_skeleton_reconnected.png`.

- every green connector is protected unconditionally;
- a red/black connector is protected when its red/black alternate path forms
  a bounding box of at least `500 px²`;
- for every other connector, only pixels solely owned by that connector are
  temporarily removed;
- no full-graph alternate path means the connector is required and restored;
- an alternate-path loop below `500 px²` deletes the connector;
- a larger loop restores the connector;
- connector ownership is counted, so deleting one connector does not erase a
  pixel still owned by another connector.

The result is `03b_skeleton_small_loop_pruned.png`.

Final short-branch cleanup traces the surviving full graph, including all
backfilled connector pixels and green connectors. Branch length is Euclidean
polyline arc rather than point count. Branches up to `60 pixels` are removed
unless at least half of the would-be-deleted samples classify as red or black
in the effective source-plus-connector color map.

The result is `03_skeleton_clean.png`. Step 4 receives a matching effective
color map, so surviving synthetic pixels keep their connector color instead
of being inferred from background.

Outputs:

```text
03a_red_black_connectors.png
03a_red_black_reconnected.png
03a_skeleton_connected.png
03d_skeleton_reconnected.png
03b_skeleton_small_loop_pruned.png
03_skeleton_clean.png
03b_connector_prune_debug.json
```

The JSON records connector stage, color, endpoints, gap length, source and
target angles, target type, alternate-path results, protection state, and
final keep/delete decision.

### 15.5 Step 4: graph tracing and color splitting

- minimum traced path size: `3 points`;
- graph-node pass traces endpoint/branch-to-node paths;
- a second pass traces remaining edges, including pure cycles;
- at choices, continuation maximizes direction dot product.

Per-point labels are grouped into color runs. Step 3 connector pixels already
carry their source connector color. Any remaining unclassified runs are
resolved from neighbors:

- same/single neighbor color: use that color;
- primary vs primary: use the directionally better aligned neighbor;
- red with black: black wins;
- green with black: green wins;
- blue with black: blue wins;
- no colored neighbors: black.

This red/black rule is important: current code does not promote mixed
red/black connector runs to red.

Short color runs below `3 pixels` of arc length are absorbed into neighbors.

Outputs:

```text
04_strokes.png
04_strokes_colored.png
04_strokes.json
```

### 15.6 Step 5: corner splitting

- direction model: PCA over left/right arc windows;
- angle threshold: `25 degrees`, unoriented;
- arc window: `30 pixels`;
- peak separation: `10 pixels`;
- maximum iterations: `5`;
- each child needs at least `3 points`;
- split points are present at adjoining segment boundaries.

Outputs:

```text
05_strokes_split.png
05_strokes_split_colored.png
05_strokes_split.json
```

### 15.7 Step 6: same-color merge

- only identical color classes merge;
- max endpoint gap: `3 pixels`;
- max unoriented angle: `12 degrees`;
- junction protection radius: `3 pixels`;
- maximum merge iterations: `80`;
- candidate cost: gap plus angle penalty.

Outputs use stem `06_merged`.

### 15.8 Step 7: metrics

Each stroke receives:

- arc length;
- endpoint chord;
- straightness `chord / arc`;
- PCA principal direction;
- PCA RMS error;
- PCA 90th-percentile error;
- chord-line 90th-percentile deviation;
- chord deviation ratio.

Outputs use stem `07_stroke_info`.

### 15.9 Step 8: enclosed preview

- raster barrier thickness: `3`;
- every endpoint connects to its nearest endpoint;
- there is no maximum endpoint-connection distance;
- four-neighbor flood fill begins at image borders;
- unreached non-barrier pixels are enclosed.

Outputs:

```text
08a_enclosed_barrier.png
08_enclosed_mask.png
```

Step 8 is diagnostic. Step 9 builds a separate planar graph and does not
consume this mask.

## 16. Step 9: Planar Red/Black Graph

Only red and black strokes participate in cap topology. Green is used after
candidate loops exist. Blue is ignored from this point onward.

### 16.1 Planarization

All real red/red and red/black segment intersections are found, including
boundaries of collinear overlaps. Both strokes are split at those intersection
parameters. Real black/black crossings are not converted into topology nodes.

The initial red topology uses exact endpoint coordinates, with
`CapGeometryEpsilon = 1e-6`; it does not use the later `5-pixel` graph snap.
Only red nodes with red degree `1` become repair sources. Degree-2 chain/loop
nodes, branches, and interior vertices never enter the dead-end pool.

Endpoint outward direction is measured from approximately `5 pixels` of arc.
Every nonzero search vector must remain in the source endpoint's forward
half-plane:

```text
dot(target - endpoint, outward_direction) >= 0
```

There is no 60-degree cone. A direction almost perpendicular to the endpoint
trend remains eligible as long as it is not behind the endpoint.

The repair constants are:

```text
black contact distance = 2 px
maximum connector distance = ConnectorTol = 20 px
black preference bias = 2 px
```

#### 16.1.1 Stage A: black contact

Every initial red dead end first searches all real black segments:

1. distance `<= 2 px`;
2. an exact contact is accepted regardless of direction;
3. a nonzero contact must satisfy the forward-half-plane test;
4. the nearest viable black contact is retained.

An exact contact requires no synthetic edge because initial red/black
planarization already created a shared node. A nonzero contact creates a red
synthetic connector from the red endpoint to the closest black point.

Black-contact proposals are sorted by distance and stable endpoint IDs. A
connector is rejected if, before reaching its intended endpoint, it intersects
real red/black geometry, or if it intersects an already accepted connector
anywhere except a shared endpoint.

Accepted black contacts are removed from red endpoint pairing.

#### 16.1.2 Stage A: red endpoint pairing

All remaining red dead ends are compared globally. A pair is eligible when:

1. endpoint distance is greater than zero and at most `20 px`;
2. A sees B in A's forward half-plane, or B sees A in B's forward half-plane;
3. the direct connector path is geometrically clear.

The target endpoint does not need to face the source endpoint. Pairing the two
ends of the same original red stroke is allowed.

Each endpoint independently records its nearest viable ordinary black-segment
candidate. A pair of distance `pairDistance` is vetoed when either endpoint
satisfies:

```text
blackDistance + 2 px < pairDistance
```

Equality does not make black win.

All surviving pair edges are sorted by:

1. distance;
2. source dead-end ID;
3. target dead-end ID.

The pass greedily accepts an edge only if neither endpoint has already been
occupied. This makes the result deterministic and prioritizes the shortest
local repairs, although it is not a maximum-cardinality matching algorithm.
Both endpoints of an accepted pair are marked together.

#### 16.1.3 Stage A commit and temporary graph

Black-contact and endpoint-pair connectors are committed in one Stage A
geometry set. Planarization is then repeated with these rules:

- real red/red and red/black intersections are split;
- every intersection involving a synthetic connector is split on both sides;
- real black/black crossings remain unsplit;
- real green/blue strokes do not participate in cap topology.

Real fragments are emitted first and synthetic fragments afterward, preserving
the `FirstSyntheticStrokeId` contract.

An exact-coordinate mixed red/black graph is built from this geometry. Only
the original red dead-end IDs are checked again; new synthetic fragment
endpoints never become new repair sources. An original endpoint is unresolved
only when its mixed-graph node still has total degree `1`. Thus an endpoint
connected to black remains red-degree `1` but is correctly considered solved.
The final `5-pixel` snap is deliberately not used here.

#### 16.1.4 Stage B: independent segment search

Every unresolved original endpoint searches two candidate classes from the
same Stage A snapshot.

Real red candidate:

- distance at most `20 px`;
- source forward-half-plane test;
- excludes the endpoint's own original source stroke;
- target must be in the strict interior of a real red stroke;
- synthetic red connectors are not target geometry.

Black candidate:

- distance at most `20 px`;
- source forward-half-plane test;
- may target a real black endpoint or interior segment point.

The two searches are independent. The endpoint's single result is:

```text
only red candidate       -> red
only black candidate     -> black
both candidates:
    blackDistance + 2 px < redDistance -> black
    otherwise                         -> red
neither candidate        -> unresolved
```

Stage B proposals are sorted by distance and stable IDs. Multiple connectors
may target the same real point, but duplicate connector edges are removed.
A later proposal is rejected if its path intersects unrelated real geometry
before its target, passes through an existing connector, or crosses an earlier
accepted proposal. Rejected proposals do not trigger another candidate search.

Synthetic red connectors participate in temporary/final topology,
planarization, `red_only`, `local_black`, and `fallback_trace`; they are
excluded only as active red-segment targets in Stage B.

#### 16.1.5 Black endpoint repair and final planarization

After Stage B is committed and planarized, real black endpoints whose exact
mixed-graph degree is still `1` may connect forward to nearby red/black
endpoints within `20 px`. These black proposals are also batched, distance
sorted, deduplicated, and checked against real geometry and accepted
connectors.

The complete source geometry and all accepted red/black connectors are then
planarized once more. Synthetic connector intersections become real graph
nodes. There is no unbounded repair iteration: red repair has exactly Stage A
and Stage B, followed by the black endpoint pass.

Only after this process do graph endpoints snap into shared nodes within
`5 pixels` for cap-loop extraction.

Root diagnostic outputs:

```text
09_all_red_black_graph.png
09_all_red_black_graph.json
```

### 16.2 Candidate generation

Three passes generate cycles:

1. `red_only`, priority 0;
2. each red connected component plus the global black pool as `local_black`,
   priority 1;
3. all red and black as `fallback_trace`, priority 2.

Only a real, non-synthetic red stroke with arc length at least `20 pixels` may
act as a cycle anchor. Synthetic red connectors may still participate in the
alternate path.

For each eligible anchor:

1. remove that edge;
2. begin at the minimum alternate-path graph-edge count between its endpoints;
3. enumerate all simple alternate paths at that exact edge count;
4. combine each alternate path and the removed anchor into a cycle;
5. if no path at that depth produces a valid candidate for the current pass,
   advance one edge depth and repeat;
6. stop after the first depth containing at least one valid candidate.

Longer paths are not retained merely to fill a second slot. If there is only
one valid cycle at the first valid edge depth, the anchor produces one
candidate. If multiple valid cycles tie at that depth, the anchor retains at
most two distinct stroke-set cycles, ordered by:

1. fewer cycle graph edges;
2. greater total non-synthetic real-red arc length;
3. shorter total black arc length;
4. smaller absolute polygon area;
5. lexical stroke-set key for deterministic final tie breaking.

For `local_black`, only cycles that actually contain black boundary strokes
count toward the two retained candidates.

A valid cycle:

- has at least two graph edges;
- has at least one non-synthetic red stroke;
- produces a polygon with at least four samples;
- has polygon area greater than `1`.

Ordered boundary-run metadata is retained for Step 10:

- stroke ID and color;
- synthetic/reversed flags;
- start/end graph node IDs and positions;
- oriented polyline samples;
- arc, chord, and straightness metrics.

Synthetic connectors retain cyclic topology but are later ignored as fitted
geometry.

### 16.3 Interior-red rejection

After green prefiltering, a candidate is rejected when red geometry not used
as its cap boundary contributes an interior segment longer than `20 pixels`.

### 16.4 Candidate minimum size

Current code uses:

- red-only minimum bounding-box area: `500 px²`;
- borrowed-black/fallback minimum bounding-box area: `500 px²`.

Both values are constants in `FromLZImageOps.cpp`.

## 17. Step 9: Green Chain and Action

### 17.1 Local green discovery

A green stroke becomes a local seed when either endpoint is within
`BlackSelectTol = 20 pixels` of any point belonging to the candidate's real
red strokes.

Locality is not measured against borrowed black geometry.

### 17.2 Bidirectional chain tracing

For every local seed:

- trace from both endpoints;
- connect to green endpoints within `10 pixels`;
- require continuation angle less than `45 degrees`;
- choose the highest direction dot, then smallest gap, then lowest stroke ID;
- never revisit a green stroke;
- total path length is stroke arcs plus inter-stroke gaps;
- orient the completed chain from the endpoint nearest the cap to the endpoint
  farthest from the cap.

Duplicate chains are removed by their sorted stroke-ID set.

Best-chain order:

1. greatest path length;
2. greatest endpoint chord when path lengths tie.

The side vector is:

```text
side_vector = chain_end - chain_start
```

The translated cap is every cap point plus this vector.

### 17.3 Attach/excavate decision

The cap boundary is rasterized at one-pixel thickness and border flood fill
builds the interior.

For every local green polyline:

- each rasterized pixel step contributes its Euclidean pixel length;
- a boundary step contributes to inside, outside, and boundary totals;
- an ambiguous crossing contributes to both inside and outside.

Decision:

- inside length greater than outside: `excavate`;
- outside greater than inside: `attach`;
- equal or no local green: `skip`.

`InteriorGreenMinInsideLengthPx = 10` is written for compatibility/diagnostics.
It does not gate the final attach/excavate comparison.

## 18. Step 9: Face Prevalidation Before Selection

Every loop candidate is sent to
`FFromLZFaceReconstructor::EvaluateCandidateFaces` before red-stroke ownership
is assigned.

Source polygon semantics:

- excavate: `cap_polygon`;
- attach: `cap_polygon_translated`.

The candidate is mapped from Step 9 image size to the captured face image and
rasterized. A face survives only when its ID color occupies at least `25%` of
the entire source polygon mask.

For each surviving face:

1. intersect the overlap centroid ray with the face plane;
2. project the face normal into image space;
3. compare it to every candidate side vector using the unoriented angle;
4. require angle at most `30 degrees`;
5. prefer angle strictly below `10 degrees`;
6. within preferred or fallback group, select the face nearest the camera.

The result, including selected face ID, overlap, angle, distance, and reject
reason, is stored in the candidate and Step 9 cap JSON.

Per-candidate validation artifacts are under:

```text
CandidateFaceValidation/Candidate_###_<source>/
```

### 18.1 Final candidate validation and classification

Before selection every candidate must pass:

- valid local green attach/excavate action;
- minimum bounding-box area;
- interior-red check;
- valid face prevalidation.

Semantic classification uses the actual selected boundary:

- no black boundary strokes: authoritative red-only;
- one or more black boundary strokes: mixed.

`CandidateSource` remains diagnostic. For example, a pure-red fallback
candidate is still classified as authoritative red-only.
Every valid authoritative red-only loop is selected. Its real red strokes are
locked for ownership. Red-only loops are expected not to share real red
strokes; if this invariant is violated, all valid red-only loops remain
selected and a warning is recorded.

Valid mixed candidates enter the pool only when none of their real red strokes
are locked by an authoritative red-only loop.

### 18.2 Mixed candidate global selection

Valid `local_black` and `fallback_trace` candidates enter one pool. Two mixed
candidates conflict only when they share a real red stroke. Polygon overlap,
containment, or intersection does not create a conflict.

Selection first builds a deterministic greedy feasible solution, then runs
branch-and-bound with a single `5 second` wall-clock budget for the whole mixed
pool.

The combination objective is lexicographic:

1. maximize unique covered real-red arc length;
2. minimize total borrowed-black arc length;
3. minimize total graph-edge count;
4. maximize unique covered real-red stroke count;
5. minimize selected candidate count;
6. maximize selected `local_black` candidate count;
7. use the sorted candidate-key sequence for deterministic tie breaking.

If search finishes within the budget, the mixed result is the exact global
optimum. If it times out, the best combination found so far is retained and
the root `selection` object records `mixed_timed_out: true` and
`mixed_optimality_proven: false`. Selected mixed candidates use
`selection_phase: "mixed_best_found_timeout"`; unselected mixed candidates use
`selection_phase: "mixed_not_selected"` with a timeout-specific `reason`.
Authoritative red-only output is never discarded because mixed search timed
out.

Root output: `09_loop_candidates.json`.

## 19. Step 9 Component Outputs

Selected candidates are numbered in final selection order:

```text
Saved/2DDebug/Press_##/Component_##
Saved/FromAction/Press_##/Component_##
```

Debug component files:

```text
09a_caploop_candidate.png
09a_caploop_candidate_graph.json
09b_caploop_pruned.png
09b_caploop_pruned_graph.json
09_cap_extrusion.png
09_cap_extrusion.json
```

Action file:

```text
Saved/FromAction/Press_##/Component_##/Action.json
```

`09_cap_extrusion.json` is the main Step 9-to-Step 10 contract. It includes:

- action and decision statistics;
- cap/translated polygons;
- side vector, side segments, chain IDs, path/gap metrics;
- candidate source/anchor;
- cap stroke/node IDs;
- ordered boundary runs;
- face-prevalidation result.

## 20. Step 10 Common Input Validation

`ProcessPress` enumerates `Component_*` folders and loads common data once.

Required:

- `capture_ref.json`;
- face PNG and face JSON;
- capture JSON with orthographic view and usable matrix;
- unique face-color mapping.

Optional:

- actor/material ID PNG and JSON.

Projection comparison uses only:

```text
M00, M11, M30, M31
```

Relative tolerance is `1e-8` with scale `max(1, |a|, |b|)`.

The capture and face metadata must match. If actor/material metadata loaded,
it must also match. A mismatch aborts every component instead of silently
approximating coordinates.

Common failures still create component failure JSON, Step 10 skipped-solid
JSON, regularization fallback diagnostics, and the press-level regularization
index.

Components are reconstructed in `ParallelFor`. Runtime actor mutation remains
on the game thread.

## 21. Step 10 Face Selection Recheck

For each component Step 10:

1. loads `Action.json`;
2. rejects `skip`, `undetermined`, and unsupported actions;
3. loads `09_cap_extrusion.json`;
4. loads cap image dimensions;
5. scales cap coordinates and side vectors to face-image space;
6. rasterizes the source polygon;
7. repeats the same `25%`, `30-degree`, `10-degree`, nearest-camera selection;
8. requires the selected face ID to exactly equal Step 9's
   `preselected_face_id`.

This duplicate check prevents Step 9 and Step 10 from reconstructing different
faces after serialization or coordinate conversion.

Outputs:

```text
10_cap_mask.png
10_face_overlap.png
10_normal_green_check.png
10_face_reconstruction.json
```

The selected face key-point polygon is triangulated and oriented toward the
camera for diagnostics. It is not the generated solid cap.

## 22. Pixel/World Projection

For image pixel center `(x, y)`:

```text
ndcX = 2 * ((x + 0.5) / width) - 1
ndcY = 1 - 2 * ((y + 0.5) / height)

right = (ndcX - M30) / M00
up    = (ndcY - M31) / M11

rayOrigin = cameraLocation + cameraRight * right + cameraUp * up
rayDirection = cameraForward
```

The ray is intersected with the selected face plane.

World projection performs the inverse operation with the same camera basis and
matrix. Points behind the camera are rejected.

## 23. Paired Loop Cleanup

The source and copied polygons must initially have equal point counts and at
least three points.

Cleanup is paired so indices remain corresponding:

1. remove duplicate closing pair;
2. remove coincident adjacent pairs;
3. repeatedly remove points collinear in both loops, using approximately
   `0.75 pixels`;
4. when above `384` points, closed-loop RDP with tolerance `1.25 pixels`;
5. decimate further to target at most `384`.

The average paired delta after cleanup is the source-to-copied image vector.
It must be non-zero.

## 24. Step 10 World-Octilinear Regularization

Regularization is attempted after the original source loop is unprojected.
Failure restores the original cap and does not cancel reconstruction.

Although legacy names still contain `BBox`, the active function is
`RegularizeCapToWorldOrthogonalPolygon`. It supports U, V, +45-degree, and
-45-degree octilinear directions.

### 24.1 Runtime CVars

```text
r.FromLZ.UsePerFaceCapture
```

- default `1`;
- detects shared-camera contour points clipped by the image rectangle;
- snaps those clip corners to the axis-aligned face-UV bounds;
- replaces the debug/reference face boundary when successful;
- `0` keeps the raw shared-camera contour.

```text
r.FromLZ.PureRedAllowDiagonalRoot
```

- default `0`;
- pure-red root hypotheses may target only U/V;
- non-zero restores diagonal root targets.

### 24.2 World-aligned face basis

The world axis most parallel to the face normal is excluded. The remaining
world axes are projected into the face plane to form an oriented U/V basis.

All UV values are in world units. Pixel distances for validation are measured
by projecting UV points back through the exact camera and converting to cap
space.

### 24.3 Boundary-run semantics

Runs are translated for attachment source semantics before unprojection:

- excavate source run translation: zero;
- attach source run translation: Step 9 side vector.

Connector runs:

- preserve cyclic run ordering and provenance;
- are counted in diagnostics;
- contribute no fitted geometry.

Black runs:

- become one primitive edge per run;
- retain graph node IDs and original node UV positions.

Red runs:

- are simplified/protected into primitive edge fragments;
- significant turns and directional macro groups are retained.

Current constants:

- black assigned-axis angle tolerance: `5 degrees`;
- U/V-vs-diagonal classification threshold: `40 degrees`;
- black node snap budget: `10 pixels`;
- red macro corridor: `5 pixels`;
- red macro minimum length: `20 pixels`;
- red primitive RDP tolerance: `1 pixel`;
- short red edge threshold: `20 pixels`;
- minimum corrected/original area ratio: `0.4`;
- maximum pure-red root candidates: `5`.

### 24.4 Primitive classification

Each primitive is assigned one of:

```text
U
V
DiagPlus
DiagMinus
```

Short adjacent red primitives may collapse. Adjacent primitives with the same
octilinear axis are merged while preserving source run/stroke/primitive
provenance.

Every black edge must require at most `5 degrees` of rotation to its assigned
axis.

### 24.5 Pure-red root hypotheses

When no black run exists:

1. compute area centroid of the original cap;
2. consider reliable primitive roots with:
   - path length at least `20 pixels`;
   - max chord deviation at most `5 pixels`;
3. rank root candidates and keep at most five;
4. generate rotation hypotheses to the root's assigned octilinear target;
5. by default skip diagonal target roots;
6. rotate all primitive/sample geometry around the cap centroid;
7. solve and validate every hypothesis;
8. select the best valid hypothesis by solve quality.

Diagnostics retain every hypothesis, root stroke IDs, target axis, rotation,
angle cost, area ratio, and boundary distance.

When black exists, phase is locked to world axes and root rotation is zero.

### 24.6 Adjacency and support solve

After same-axis merge:

- at least three support edges are required;
- adjacent edges may not have the same axis type;
- weighted squared angular cost is diagnostic.

Black support:

- support coordinate is the weighted median of samples;
- graph nodes are projected to that support;
- every node movement must be at most `10 pixels`.

Red support:

- between two black neighbors: average their fixed coordinates;
- after one black neighbor: use that fixed coordinate;
- otherwise: weighted median of red samples.

### 24.7 Vertex solve

Every output vertex is the analytic intersection of the previous/current
octilinear support lines.

For pure red, the solved polygon is translated so its area centroid matches
the original cap centroid.

Black graph nodes are checked again against final solved corners/supports and
must remain within the `10-pixel` snap budget.

### 24.8 Hard validation and diagnostic-only metrics

Hard rejection:

- corrected area ratio is non-finite or below `0.4`;
- self-intersection;
- reversal of a protected convex/concave turn;
- failed world conversion;
- failed orthographic reprojection;
- any earlier stage-specific failure.

Boundary mean/max distance and angle cost are currently diagnostic only. There
is no active upper boundary-distance threshold and no upper area-ratio limit.

This is a major difference from earlier implementations/documentation.

### 24.9 Copied loop after correction

The original average source-to-copy image delta is preserved:

```text
copiedPixel = project(correctedSourceWorld) + originalDeltaPixels
```

The corrected source pixels and copied targets then continue through the
ordinary extrusion-depth solver.

### 24.10 Stage state machine

Every regularization attempt tracks:

```text
input_validation
world_aligned_face_uv
boundary_run_unprojection
red_corner_protection
primitive_edge_classification
pure_red_root_alignment
same_axis_merge
adjacency_validation
support_line_solve
vertex_intersections
topology_validation
metric_validation
world_conversion
orthographic_reprojection
```

Statuses are `not_reached`, `in_progress`, `passed`, or `failed`.

## 25. Regularization Debug Artifacts

Every component receives, even when regularization is not reached:

```text
10_cap_world_orthogonal_overview.png
10_cap_world_orthogonal_detail.png
10_cap_world_orthogonal_regularization.json
10_cap_world_orthogonal_regularization_world.json
10_cap_world_orthogonal_steps/
```

The steps directory contains:

```text
trace.json
00_world_uv_runs.json/.png
01_protected_corners.json/.png
02_primitive_edges.json/.png
02a_root_alignment.json/.png
03_same_axis_merged.json/.png
04_axis_validation.json/.png
05_support_lines.json/.png
06_corrected_polygon.json/.png
07_final_validation.json/.png
```

The press root contains:

```text
10_cap_world_orthogonal_index.json
```

It summarizes attempted/applied/fallback counts and each component's failed
stage, reason, selected geometry, and debug directory.

## 26. Extrusion Depth

The face normal is oriented so its projected image direction points toward the
source-to-copy vector.

For oriented world normal `n`:

```text
pixelPerWorld.x = dot(n, cameraRight) * 0.5 * width  * M00
pixelPerWorld.y = -dot(n, cameraUp)   * 0.5 * height * M11

depth = dot(pixelPerWorld, sourceToCopy)
      / dot(pixelPerWorld, pixelPerWorld)
```

The depth must be finite and greater than `0.0001`.

Each copied world point is:

```text
sourceWorld + orientedNormal * depth
```

Per-vertex allowed reprojection error:

```text
max(25 pixels, 0.75 * sourceToCopyLength)
```

At least three depth samples must pass. Final max copied-loop error above the
threshold becomes a warning, not a hard rejection, after the minimum valid
sample check.

## 27. Closed Solid Construction

The source cap is triangulated by ear clipping. If no ear can be clipped, a
triangle fan is emitted.

Before triangle emission, paired loops may be reversed so source-cap winding
is opposite the extrusion direction.

The mesh contains:

- source cap;
- copied cap with reversed triangle order;
- two side triangles per polygon edge.

Step 10 outputs:

```text
10_solid_reconstruction.json
10_solid_projection_check.png
```

Successful excavation cutters are enlarged only when converted to runtime
spawn data:

- along extrusion normal: `1.2`;
- perpendicular to normal: `1.1`.

## 28. Attachment Material Recovery

Primary path:

1. rasterize the source polygon in actor/material image space;
2. optionally restrict samples to the selected face;
3. count valid ID colors;
4. choose the majority entry;
5. resolve a live component using path/name/type scoring;
6. retrieve its material slot.

Runtime-generated active attachments and Boolean results may be material
sources; cutters and inactive runtime actors may not.

Fallback path:

- convert candidate visible scene meshes;
- calculate average point-to-triangle distance from source loop probes plus
  centroid;
- use the nearest plausible component/material.

If both fail, the generated attachment uses the project/debug material path
and vertex color.

## 29. Game-Thread Spawn Order

`SpawnMeshesOnGameThread` first checks world validity and session generation.

The important operation order is:

1. collect excavation cutters from the current reconstructed mesh list;
2. apply Step 11 excavation Booleans to eligible existing targets;
3. spawn all current reconstructed actors;
4. hide accepted current cutters;
5. export the reconstruction scene OBJ.

Therefore, a current-press attachment is not cut by a current-press
excavation. It becomes an eligible target in a later press.

Spawned reconstruction actors receive:

- reconstructed-solid tag;
- action tag for attachment or cutter;
- generated-by-press actor/component tags.

Procedural reconstruction drawing duplicates triangle vertices for flat
normals and emits UE-facing winding.

## 30. Step 11 Boolean Target Classes

Eligible targets:

- visible base static mesh components;
- active attachment procedural meshes from previous presses;
- active prior Boolean-result procedural meshes.

Excluded:

- reconstruction debug face actors;
- excavation cutters;
- inactive/undone runtime actors;
- hidden or empty components.

Bounds are checked first:

- exact bounds intersection;
- bounds expanded by `1 cm` for diagnostics/near-overlap handling.

Static and procedural components are converted to world-space
`FDynamicMesh3`. Vertices are welded by quantized position at `0.01 cm`.

## 31. Manifold3D Difference

Backend:

```text
Manifold3D 3.5.0+37125da
```

Conversion rules:

- Manifold input tolerance: `0.001 cm`;
- vertex properties contain XYZ only;
- degenerate triangles are skipped;
- `MeshGL64::Merge()` is called;
- signed-negative target/cutter meshes are reversed before Manifold;
- output vertices are welded at `0.01 cm`;
- output orientation is restored to the requested render sign.

Operation:

```text
result = target - cutter
```

The primary attempt uses target render orientation. A fallback attempt reverses
the target for the pass.

## 32. Boolean Acceptance

Before Manifold, cutter diagnostics require:

- triangles present;
- zero boundary edges;
- zero non-manifold edges.

A non-empty result must have:

- zero boundary edges;
- zero non-manifold edges;
- minimum edge length at least `0.05 cm`;
- volume reduction at least:

```text
max(1 cm³, targetAbsVolume * 0.0001)
```

An empty result is accepted as complete removal.

After accepted difference:

- base source components are hidden and tagged by press;
- prior procedural result actors may be replaced/hidden according to target
  type;
- a new Boolean-result procedural actor is created;
- accepted cutter names are recorded and those cutter actors are hidden after
  spawn.

Boolean result rendering uses flat procedural sections. Source/cutter section
provenance is retained where available so materials can be assigned to
separate sections.

Press-level output:

```text
11_boolean_diagnostics.json
```

## 33. Undo and Runtime Tagging

Active press IDs are stored in a stack. The latest ID is also cached for
compatibility.

Undo:

1. pop the newest active press;
2. find generated actors carrying that press tag;
3. destroy generated attachment, cutter, and Boolean-result actors from that
   press;
4. remove that press's hidden-by tags from source components;
5. restore visibility when no other active press still hides the component;
6. refresh active runtime state.

Output:

```text
Saved/2DDebug/Press_##/11_undo_diagnostics.json
```

Capture includes only active runtime attachments and Boolean results.

## 34. Full Session Reset

The global Tab handler:

1. rejects a missing world;
2. ignores repeated Tab while pending;
3. marks reset pending;
4. increments session generation immediately;
5. cancels pending capture;
6. closes the board;
7. waits for all composite workers to finish.

Finalization:

1. cancel capture/close board again defensively;
2. restore all active presses in reverse order;
3. destroy all remaining runtime-tagged actors;
4. clear runtime visibility/tags and undo stack;
5. copy working folders to a unique
   `Saved/log_YYYYMMDD_HHMMSS[_NN]`;
6. delete and recreate working folders;
7. reload the current level.

Archived folders:

```text
2DDebug
FromAction
FromLZCaptures
FromProcess
FromSketch
Logs
```

Unlike normal module startup, a full reset clears `FromSketch`.

## 35. Stroke and Action Semantics

- Red: intended cap topology and semantic boundary.
- Black: captured scene contour that can close/anchor a red cap.
- Green: local translation chain and attach/excavate classifier.
- Blue: parsed, split, merged, measured, and debugged through Step 8, but has
  no Step 9/3D meaning.

Actions:

- `attach`: source plane is under the translated cap; extrusion returns toward
  the original cap and the solid is spawned.
- `excavate`: source plane is under the original cap; extrusion follows the
  translated cap and the enlarged solid becomes a Boolean cutter.

## 36. Output Tree

Typical successful press:

```text
Saved/
  2DDebug/
    Press_01/
      capture_ref.json
      00_input.png ... 08_enclosed_mask.png
      09_all_red_black_graph.*
      09_loop_candidates.json
      CandidateFaceValidation/
      Component_01/
        09*.png/json
        10_face_reconstruction.json
        10_cap_mask.png
        10_face_overlap.png
        10_normal_green_check.png
        10_solid_reconstruction.json
        10_solid_projection_check.png
        10_cap_world_orthogonal_*
        10_cap_world_orthogonal_steps/
      10_cap_world_orthogonal_index.json
      10_reconstruction_scene.obj
      10_reconstruction_scene.mtl
      11_boolean_diagnostics.json
  FromAction/
    Press_01/
      Component_01/
        Action.json
```

## 37. Failure Localization

Use this order:

1. `capture_ref.json`
   - Verify exact source pairing.
2. capture JSON and `_debug_transforms.json`
   - Verify selected-camera snapshot, fitted dimensions, and exact matrix.
3. main capture, depth, normal, face, and actor/material images
   - Separate line-art/segmentation/ID failures.
4. `01` through `07`
   - Verify skeleton topology and color ownership.
5. `03b_connector_prune_debug.json`
   - Verify repair direction and loop pruning.
6. Step 8 images
   - Visual closure only.
7. `09_all_red_black_graph.*`
   - Verify planarization and connector topology.
8. `09_loop_candidates.json`
   - Check green, interior-red, face, conflict, and priority rejection.
9. `CandidateFaceValidation`
   - Check `25%` mask coverage and normal/green angle.
10. `09_cap_extrusion.json` and `Action.json`
    - Check source/copy semantics and ordered runs.
11. `10_face_reconstruction.json`
    - Check Step 9/10 face-ID agreement.
12. world-octilinear index, trace, and numbered stages
    - Find first failed stage and fallback reason.
13. `10_solid_reconstruction.json`
    - Check depth, orientation, and reprojection.
14. `11_boolean_diagnostics.json`
    - Check target/cutter manifold, bounds, attempts, edge length, and volume.

Do not tune early 2D thresholds to hide a projection, face-selection, solid
closure, or Boolean-manifold failure.

## 38. Non-Obvious Invariants

1. Capture, faces, actor/material IDs, Step 9 validation, and Step 10 must use
   the same exact orthographic projection.
2. Step 1 currently does not call morphology helpers.
3. Red/black synthetic color resolution currently chooses black.
4. Step 8 does not drive Step 9.
5. Step 9 requires valid green action and valid 3D face before selecting a
   cap.
6. Face coverage is `25%` of the whole source polygon mask.
7. Step 10 must reproduce Step 9's selected face ID.
8. Every valid no-black loop is authoritative red-only and is selected before
   mixed optimization.
9. Real red strokes locked by authoritative red-only loops cannot be reused by
   mixed candidates.
10. Mixed selection globally optimizes non-conflicting candidates for up to
    five seconds; timeout retains the best combination found.
11. Ordered boundary connectors preserve topology but are excluded from
   regularized geometry.
12. World regularization is octilinear, not only orthogonal.
13. Pure-red phase may rotate; black-containing phase is world locked.
14. Boundary distance is diagnostic only in the current regularizer.
15. A regularization failure restores the original cap and continues.
16. Source/copy order reverses between attach and excavate.
17. Current-press excavation runs before current-press attachment spawn.
18. Generated solids and cutters must be closed for Manifold.
19. Session generation prevents post-reset worker results from spawning.
20. Startup preserves sketches; full Tab reset archives and clears them.

## 39. Known Code/Comment Drift

Developers should not copy these stale descriptions without checking calls:

- `FromLZSketch2DProcessor.h` still describes morphology and has duplicate
  unchecked checklist lines, while active Step 1 performs only binarization
  and small-component removal.
- some comments describe older cap-area thresholds; active Step 9 constants
  are `500 px²` for both red-only and borrowed loops.
- older documentation describes different face-overlap thresholds; active code
  requires `25%`.
- older documentation treated black endpoints as immutable; current
  octilinear code permits support snapping within `10 pixels`.
- older documentation described upper area and boundary-distance rejection;
  current hard metric check only enforces area ratio at least `0.4`.

When changing parameters, update both this guide and the nearby stale source
comments so future readers do not have two competing pipelines.
