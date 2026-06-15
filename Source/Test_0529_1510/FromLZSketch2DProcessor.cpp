#include "FromLZSketch2DProcessor.h"

#include "Async/Async.h"
#include "FromLZFaceReconstructor.h"
#include "FromLZImageOps.h"
#include "FromLZPressNaming.h"
#include "FromLZSessionReset.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

void FFromLZSketch2DProcessor::ProcessCompositeAsync(TArray<uint8> RGBA, int32 Width, int32 Height, const FString& DebugDir, const FSketchSourceInfo& Source, UWorld* World)
{
	// RGBA is taken by value; move it into the async task so the heavy work runs off the game thread.
	const FString DebugDirCopy = DebugDir;
	const FSketchSourceInfo SourceCopy = Source;
	const TWeakObjectPtr<UWorld> WorldCopy(World);
	const int32 SessionGeneration = FFromLZSessionReset::GetSessionGeneration();
	TArray<uint8> Pixels = MoveTemp(RGBA);

	FFromLZSessionReset::NotifyCompositeTaskStarted();
	Async(EAsyncExecution::ThreadPool, [Pixels = MoveTemp(Pixels), Width, Height, DebugDirCopy, SourceCopy, WorldCopy, SessionGeneration]() mutable
	{
		FFromLZSketch2DProcessor::ProcessCompositeWithGeneration(Pixels, Width, Height, DebugDirCopy, SourceCopy, SessionGeneration, WorldCopy);
		FFromLZSessionReset::NotifyCompositeTaskFinished();
	});
}

bool FFromLZSketch2DProcessor::ProcessComposite(const TArray<uint8>& RGBA, int32 Width, int32 Height, const FString& DebugDir, const FSketchSourceInfo& Source, TWeakObjectPtr<UWorld> World)
{
	return ProcessCompositeWithGeneration(RGBA, Width, Height, DebugDir, Source, FFromLZSessionReset::GetSessionGeneration(), World);
}

bool FFromLZSketch2DProcessor::ProcessCompositeWithGeneration(const TArray<uint8>& RGBA, int32 Width, int32 Height, const FString& DebugDir, const FSketchSourceInfo& Source, int32 SessionGeneration, TWeakObjectPtr<UWorld> World)
{
	if (Width <= 0 || Height <= 0 || RGBA.Num() < Width * Height * 4)
	{
		UE_LOG(LogTemp, Warning, TEXT("Sketch2D: invalid input (%dx%d, %d bytes)"), Width, Height, RGBA.Num());
		return false;
	}
	if (!FFromLZSessionReset::IsSessionGenerationCurrent(SessionGeneration))
	{
		UE_LOG(LogTemp, Log, TEXT("Sketch2D: skipped stale composite because the session generation changed before processing started."));
		return false;
	}

	IFileManager::Get().MakeDirectory(*DebugDir, true);

	// Each pipeline run (one Space press) gets its own Press_## folder. Number continues
	// from the highest existing Press_* so runs accumulate across restarts.
	const FString PressName = FFromLZPressNaming::MakePressName(FFromLZPressNaming::GetNextPressIndex(DebugDir));
	const FString PressDir = DebugDir / PressName;
	IFileManager::Get().MakeDirectory(*PressDir, true);

	// Action.json outputs live under Saved/FromAction/Press_##/ (sibling of 2DDebug).
	const FString ActionPressDir = FPaths::GetPath(DebugDir) / TEXT("FromAction") / PressName;
	IFileManager::Get().MakeDirectory(*ActionPressDir, true);

	// Record which FromLZCaptures / FromSketch source files this press consumed
	// (paths relative to Saved/). Written into the Press_## folder as capture_ref.json.
	{
		const FString ProcessedAt = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString RefJson;
		RefJson += TEXT("{\n");
		RefJson += FString::Printf(TEXT("  \"press\": \"%s\",\n"), *PressName);
		RefJson += FString::Printf(TEXT("  \"processed_at\": \"%s\",\n"), *ProcessedAt);
		RefJson += FString::Printf(TEXT("  \"has_capture\": %s,\n"), Source.bHasCapture ? TEXT("true") : TEXT("false"));
		RefJson += FString::Printf(TEXT("  \"capture_stem\": \"%s\",\n"), *Source.CaptureStem);
		RefJson += FString::Printf(TEXT("  \"capture_png\": \"%s\",\n"), *Source.CapturePngRel);
		RefJson += FString::Printf(TEXT("  \"capture_json\": \"%s\",\n"), *Source.CaptureJsonRel);
		RefJson += FString::Printf(TEXT("  \"faces_png\": \"%s\",\n"), *Source.FacesPngRel);
		RefJson += FString::Printf(TEXT("  \"faces_json\": \"%s\",\n"), *Source.FacesJsonRel);
		RefJson += FString::Printf(TEXT("  \"actor_material_png\": \"%s\",\n"), *Source.ActorMaterialPngRel);
		RefJson += FString::Printf(TEXT("  \"actor_material_json\": \"%s\",\n"), *Source.ActorMaterialJsonRel);
		RefJson += FString::Printf(TEXT("  \"sketch_png\": \"%s\"\n"), *Source.SketchPngRel);
		RefJson += TEXT("}\n");
		FFileHelper::SaveStringToFile(RefJson, *(PressDir / TEXT("capture_ref.json")));
	}

	const double StartTime = FPlatformTime::Seconds();

	// ---- Step 1: preprocess -------------------------------------------------
	// Non-white -> foreground (replaces grayscale+Gaussian+Otsu for the colored composite),
	// then remove small components without changing the source-stroke topology.
	TArray<uint8> Bin;
	FromLZImageOps::BinarizeNonWhite(RGBA, Width, Height, /*WhiteThreshold*/ 240, Bin);
	FromLZImageOps::RemoveSmallComponents(Bin, Width, Height, /*MinArea*/ 12);

	// 00_input is the raw composite (saved for reference), 01_binary is the cleaned mask.
	{
		TArray<uint8> InputRGBA = RGBA;
		IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> IW = IWM.CreateImageWrapper(EImageFormat::PNG);
		if (IW.IsValid())
		{
			IW->SetRaw(InputRGBA.GetData(), InputRGBA.Num(), Width, Height, ERGBFormat::RGBA, 8);
			const TArray64<uint8>& C = IW->GetCompressed();
			FFileHelper::SaveArrayToFile(TArrayView<const uint8>(C.GetData(), static_cast<int32>(C.Num())), *(PressDir / TEXT("00_input.png")));
		}
	}
	FromLZImageOps::SaveMaskPng(Bin, Width, Height, PressDir / TEXT("01_binary.png"), /*bInvertForDisplay*/ true);

	// ---- Step 2: skeletonize ------------------------------------------------
	TArray<uint8> Skel;
	FromLZImageOps::ZhangSuenThinning(Bin, Width, Height, Skel);
	FromLZImageOps::RemoveSmallComponents(Skel, Width, Height, /*MinArea*/ 6);
	FromLZImageOps::SaveMaskPng(Skel, Width, Height, PressDir / TEXT("02_skeleton.png"), /*bInvertForDisplay*/ true);

	// Per-pixel color-class map of the composite (red/green/blue/black/none).
	// Step 3 uses it to keep short dangling branches that still correspond to
	// source red/black marks; later steps reuse it for stroke color assignment.
	TArray<uint8> ColorMap;
	FromLZImageOps::BuildColorClassMap(RGBA, Width, Height, ColorMap);
	const int32 ColorSampleRadius = 2;

	// ---- Step 3: skeleton gap repair ---------------------------------------
	// Run full-skeleton connection, red/black graph reconnect, full-graph backfill,
	// connector-aware small-loop pruning, and final short-branch cleanup.
	TArray<uint8> SkelConnected, SkelReconnected, SkelSmallLoopPruned, SkelClean;
	TArray<uint8> EffectiveColorMap;
	FromLZImageOps::CleanupSkeletonEndpoints(
		Skel, Width, Height,
		/*GapTol*/ 10.0f,
		/*ConnectThickness*/ 1,
		/*SmallLoopBboxAreaThresh*/ 500.0f,
		/*BranchPruneMaxPixels*/ 0.0f, // 0 -> auto = max(30, 3*GapTol)
		ColorMap,
		ColorSampleRadius,
		PressDir / TEXT("03a_red_black_connectors.png"),
		PressDir / TEXT("03a_red_black_reconnected.png"),
		PressDir / TEXT("03b_connector_prune_debug.json"),
		SkelConnected, SkelReconnected, SkelSmallLoopPruned, SkelClean, EffectiveColorMap);
	FromLZImageOps::SaveMaskPng(SkelConnected, Width, Height, PressDir / TEXT("03a_skeleton_connected.png"), /*bInvertForDisplay*/ true);
	FromLZImageOps::SaveMaskPng(SkelReconnected, Width, Height, PressDir / TEXT("03d_skeleton_reconnected.png"), /*bInvertForDisplay*/ true);
	FromLZImageOps::SaveMaskPng(SkelSmallLoopPruned, Width, Height, PressDir / TEXT("03b_skeleton_small_loop_pruned.png"), /*bInvertForDisplay*/ true);
	FromLZImageOps::SaveMaskPng(SkelClean, Width, Height, PressDir / TEXT("03_skeleton_clean.png"), /*bInvertForDisplay*/ true);

	// The color map also keeps RGB strokes from being merged together.
	const float EndpointTol = 3.0f;
	const float ColorMinRunArc = 3.0f; // absorb short color "blips" at crossings into neighbors

	// ---- Step 4: stroke tracing + color classification --------------------
	// Crossing-number node classification + 8-connected polyline tracing, then
	// assign red/green/blue/black per stroke and split at color boundaries
	// (gap-repair runs reclassified by neighbor color/direction).
	const int32 TraceMinPixels = 3; // --trace-min-pixels
	TArray<FromLZImageOps::FStroke> Strokes;
	FromLZImageOps::TraceStrokes(SkelClean, Width, Height, TraceMinPixels, Strokes);
	FromLZImageOps::SaveStrokesPng(Strokes, Width, Height, PressDir / TEXT("04_strokes.png"));

	TArray<FromLZImageOps::FColoredStroke> ColoredStrokes;
	FromLZImageOps::ColorizeAndSplitStrokes(Strokes, EffectiveColorMap, Width, Height, ColorSampleRadius, ColorMinRunArc, ColoredStrokes);
	FromLZImageOps::SaveColoredStrokesPng(ColoredStrokes, Width, Height, PressDir / TEXT("04_strokes_colored.png"));
	FromLZImageOps::SaveColoredStrokesJson(ColoredStrokes, Width, Height, PressDir / TEXT("04_strokes.json"), EndpointTol);

	// ---- Step 5: corner splitting (color preserved, RGB never merged) -----
	TArray<FromLZImageOps::FColoredStroke> SplitColored;
	FromLZImageOps::SplitColoredStrokesAtCorners(
		ColoredStrokes,
		/*AngleThresh*/ 25.0f,            // --split-corner-angle
		/*MinPixels*/ TraceMinPixels,
		/*SegmentArc*/ 30.0f,             // --split-segment-arc
		/*SplitPeakMinDistance*/ 10.0f,   // default
		/*MaxIters*/ 5,                   // default
		SplitColored);

	// Raw per-stroke palette render (each piece a distinct color), same style as 04_strokes.png.
	{
		TArray<FromLZImageOps::FStroke> SplitGeom;
		SplitGeom.Reserve(SplitColored.Num());
		for (const FromLZImageOps::FColoredStroke& CS : SplitColored)
		{
			SplitGeom.Add(CS.Points);
		}
		FromLZImageOps::SaveStrokesPng(SplitGeom, Width, Height, PressDir / TEXT("05_strokes_split.png"));
	}
	FromLZImageOps::SaveColoredStrokesPng(SplitColored, Width, Height, PressDir / TEXT("05_strokes_split_colored.png"));
	FromLZImageOps::SaveColoredStrokesJson(SplitColored, Width, Height, PressDir / TEXT("05_strokes_split.json"), EndpointTol);

	// Helper: emit the three standard outputs for a stroke set
	// (per-stroke palette PNG, class-color PNG, detailed JSON).
	auto SaveStrokeTriplet = [&](const TArray<FromLZImageOps::FColoredStroke>& Set, const FString& Stem)
	{
		TArray<FromLZImageOps::FStroke> Geom;
		Geom.Reserve(Set.Num());
		for (const FromLZImageOps::FColoredStroke& CS : Set)
		{
			Geom.Add(CS.Points);
		}
		FromLZImageOps::SaveStrokesPng(Geom, Width, Height, PressDir / (Stem + TEXT(".png")));
		FromLZImageOps::SaveColoredStrokesPng(Set, Width, Height, PressDir / (Stem + TEXT("_colored.png")));
		FromLZImageOps::SaveColoredStrokesJson(Set, Width, Height, PressDir / (Stem + TEXT(".json")), EndpointTol);
	};

	// ---- Step 6: same-color merge ----------------------------------------
	// Only strokes of the same red/green/blue/black class are merged.
	TArray<FromLZImageOps::FColoredStroke> Merged;
	FromLZImageOps::MergeColoredStrokesSameColor(
		SplitColored,
		/*MaxGap*/ 3.0f,                 // --post-split-merge-gap
		/*MaxAngle*/ 12.0f,              // --post-split-merge-angle
		/*MaxIters*/ 80,
		/*ProtectJunctionRadius*/ 3.0f,
		Merged);
	SaveStrokeTriplet(Merged, TEXT("06_merged"));

	// ---- Step 7: stroke metrics ------------------------------------------
	// arc / chord / straightness / PCA-line errors / direction; JSON gains kind + metrics.
	FromLZImageOps::ComputeStrokeMetrics(Merged);
	SaveStrokeTriplet(Merged, TEXT("07_stroke_info"));

	// ---- Step 8: enclosed-region mask ------------------------------------
	TArray<uint8> EnclosedMask, EnclosedBarrier;
	FromLZImageOps::ComputeEnclosedRegionMask(Merged, Width, Height, /*Thickness*/ 3, EnclosedMask, EnclosedBarrier);
	FromLZImageOps::SaveMaskPng(EnclosedBarrier, Width, Height, PressDir / TEXT("08a_enclosed_barrier.png"), /*bInvertForDisplay*/ true);
	FromLZImageOps::SaveMaskPng(EnclosedMask, Width, Height, PressDir / TEXT("08_enclosed_mask.png"), /*bInvertForDisplay*/ true);

	// ---- Step 9: per-component red cap-loops -> per-component side -> translate-copy --
	// Real red/red and red/black intersections are planarized first. Exact red degree-1 endpoints
	// then run black-contact + global endpoint pairing, followed by a temporary mixed
	// graph and an independent real-red-segment/black-segment fallback pass within 20px.
	TArray<FromLZImageOps::FCapExtrusionResult> Caps;
	const int32 NumCaps = FromLZImageOps::RecoverCapExtrusionsPerComponent(
		Merged, /*ConnectorTol*/ 20.0f, /*BlackSelectTol*/ 20.0f, Width, Height, PressDir, ActionPressDir, Caps);

	// ---- Step 10: per-component action -> face-mask overlap -> runtime face rebuild --
	if (!FFromLZSessionReset::IsSessionGenerationCurrent(SessionGeneration))
	{
		UE_LOG(LogTemp, Log, TEXT("Sketch2D: skipped stale Step10/11 reconstruction because the session generation changed during processing."));
		return false;
	}
	FFromLZFaceReconstructor::ProcessPress(PressDir, ActionPressDir, World, SessionGeneration);

	const double Elapsed = FPlatformTime::Seconds() - StartTime;
	UE_LOG(LogTemp, Log, TEXT("Sketch2D: steps 1-10 done in %.3fs (%dx%d): %d merged strokes; %d cap component(s) -> %s"),
		Elapsed, Width, Height, Merged.Num(), NumCaps, *PressDir);

	return true;
}
