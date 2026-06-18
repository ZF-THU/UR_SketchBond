#pragma once

#include "CoreMinimal.h"

class UWorld;

// Migrated sketch pipeline: Steps 1-9 analyze the composite image in C++, then
// Step 10/11 are dispatched to the face reconstructor for runtime geometry.
//
// Triggered after a board Proceed/Space composite (wContextSketch_raw.png) is
// produced. Debug artifacts are written under <ProjectSaved>/2DDebug/ and
// Action.json files under <ProjectSaved>/FromAction/.
//
// Active steps:
//   [x] Step 1 preprocess (non-white binarize + remove small comps; no morphology)
//   [x] Step 2 skeletonize (Zhang-Suen + remove small skeleton comps)
//   [x] Step 3 skeleton gap repair (connect endpoints + small-loop / dangling prune)
//   [x] Step 4 stroke tracing (crossing-number nodes + 8-connected polylines)
//             + color classification (red/green/blue/black) and color-boundary split
//   [x] Step 5 corner split (PCA arc-window axis-angle + iterative NMS), color preserved
//   [x] Step 6 same-color merge (RGB/black classes never merged across colors)
//   [x] Step 7 stroke metrics (arc/chord/straightness/PCA errors/direction)
//   [x] Step 8 enclosed-region mask (endpoint-nearest-connect + flood)
//   [x] Step 9 per-component red cap-loops -> per-component longest-green side -> translate-copy
//       Each run writes to Saved/2DDebug/Press_##/, with one Component_%% subfolder per cap.
//   [x] Step 10/11 dispatch (face validation, solid rebuild, runtime spawn/Boolean)
//   Each stroke step emits a per-stroke palette PNG, a class-color PNG, and a JSON
//   (id / color / kind / metrics / endpoints / neighbors / points).
// Records which FromLZCaptures / FromSketch source files a Space press consumed.
// Paths are relative to the project's Saved/ folder (e.g. "FromLZCaptures/FromLZ_xx.png").
struct FSketchSourceInfo
{
	bool bHasCapture = false;
	FString CaptureStem;     // e.g. "FromLZ_20260531_131543"
	FString CapturePngRel;   // "FromLZCaptures/FromLZ_...png"
	FString CaptureJsonRel;  // "FromLZCaptures/FromLZ_...json"
	FString FacesPngRel;     // "FromLZCaptures/FromLZ_..._faces.png"
	FString FacesJsonRel;    // "FromLZCaptures/FromLZ_..._faces.json"
	FString ActorMaterialPngRel;  // "FromLZCaptures/FromLZ_..._actor_material_id.png"
	FString ActorMaterialJsonRel; // "FromLZCaptures/FromLZ_..._actor_material_id.json"
	FString SketchPngRel;    // "FromSketch/....png"
};

class FFromLZSketch2DProcessor
{
public:
	// Moves the RGBA buffer into the bounded background scheduler so the game
	// thread never blocks on the heavy 2D/3D processing path.
	static void ProcessCompositeAsync(TArray<uint8> RGBA, int32 Width, int32 Height, const FString& DebugDir, const FSketchSourceInfo& Source, UWorld* World);

	// Synchronous entry point (runs on the calling thread). Returns true on success.
	static bool ProcessComposite(const TArray<uint8>& RGBA, int32 Width, int32 Height, const FString& DebugDir, const FSketchSourceInfo& Source, TWeakObjectPtr<UWorld> World);

	static bool ProcessCompositeWithGeneration(const TArray<uint8>& RGBA, int32 Width, int32 Height, const FString& DebugDir, const FSketchSourceInfo& Source, int32 SessionGeneration, TWeakObjectPtr<UWorld> World);
};
