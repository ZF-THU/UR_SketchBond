#include "FromLZCaptureUtils.h"

#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/Scene.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/MaterialInterface.h"
#include "Math/Float16Color.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ProceduralMeshComponent.h"
#include "StaticMeshResources.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"

static constexpr double FromLZCaptureOrthoWidth = 1536.0;

// ===================================================================
// Normal-based planar face segmentation (Enter-key capture).
// Groups foreground pixels whose world normals are continuous and whose depth
// is continuous into planar faces, extracts each face's corner key points, and
// unprojects them to 3D world coordinates using the captured camera parameters.
// ===================================================================
namespace FromLZFaces
{
	// Perpendicular distance from P to segment-line A-B.
	static float PerpDist(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D AB = B - A;
		const float Len2 = AB.SizeSquared();
		if (Len2 < 1e-6f) { return FVector2D::Distance(P, A); }
		const float T = static_cast<float>(FVector2D::DotProduct(P - A, AB) / Len2);
		return FVector2D::Distance(P, A + AB * T);
	}

	// Ramer-Douglas-Peucker on an OPEN polyline [I0..I1]; appends interior kept indices.
	static void RDP(const TArray<FVector2D>& Pts, int32 I0, int32 I1, float Eps, TArray<int32>& OutKeep)
	{
		float MaxD = -1.f;
		int32 MaxI = -1;
		for (int32 i = I0 + 1; i < I1; ++i)
		{
			const float D = PerpDist(Pts[i], Pts[I0], Pts[I1]);
			if (D > MaxD) { MaxD = D; MaxI = i; }
		}
		if (MaxI >= 0 && MaxD > Eps)
		{
			RDP(Pts, I0, MaxI, Eps, OutKeep);
			OutKeep.Add(MaxI);
			RDP(Pts, MaxI, I1, Eps, OutKeep);
		}
	}

	// Moore-neighbor boundary trace of the region labelled R, starting at StartIdx
	// (the raster-first pixel of the region, so it is on the top boundary).
	static void TraceContour(const TArray<int32>& Label, int32 W, int32 H, int32 R, int32 StartIdx, TArray<FIntPoint>& Out)
	{
		Out.Reset();
		// Clockwise 8-neighborhood: 0=E,1=SE,2=S,3=SW,4=W,5=NW,6=N,7=NE.
		static const int32 DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
		static const int32 DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
		auto At = [&](int32 x, int32 y) -> bool
		{
			return (x >= 0 && x < W && y >= 0 && y < H) && Label[y * W + x] == R;
		};

		const FIntPoint Start(StartIdx % W, StartIdx / W);
		FIntPoint P = Start;
		Out.Add(P);
		int32 Back = 4; // came from the West (region is leftmost on its top row)
		const int32 MaxSteps = W * H * 8;
		for (int32 Step = 0; Step < MaxSteps; ++Step)
		{
			bool bFound = false;
			for (int32 k = 1; k <= 8; ++k)
			{
				const int32 d = (Back + k) & 7;
				const int32 nx = P.X + DX[d];
				const int32 ny = P.Y + DY[d];
				if (At(nx, ny))
				{
					P = FIntPoint(nx, ny);
					Back = (d + 4) & 7; // direction from the new pixel back to the old one
					bFound = true;
					break;
				}
			}
			if (!bFound) { break; } // isolated pixel
			if (P == Start) { break; }
			Out.Add(P);
		}
	}
}

// Segments the captured depth/normal buffers into planar faces, derives each face's
// corner key points (2D), unprojects them to 3D world coordinates, and writes a
// color-coded faces PNG plus a faces JSON. Reuses the already-read-back buffers.
static void SaveNormalFaces(
	const TArray<float>& Depth, const TArray<FVector3f>& Normal,
	int32 W, int32 H, const UCameraComponent* Camera,
	bool bCaptureOrthographic, double CaptureOrthoWidth,
	const FString& FacesPngPath, const FString& FacesJsonPath)
{
	using namespace FromLZFaces;

	// --- tunables ---
	const float NormalAngleTolDeg = 12.0f;
	const float CosTol = FMath::Cos(FMath::DegreesToRadians(NormalAngleTolDeg));
	const float DepthJoinTol = 0.02f;   // relative depth continuity
	const int32 MinFaceArea = 200;      // pixels
	const float RdpEps = 4.0f;          // corner simplification (px)
	const float ForegroundNormalLen = 0.1f;

	const int32 NumPx = W * H;

	// Background detection by depth: UE clears unrendered pixels to the far plane,
	// which appears as a uniform maximum depth (its normal is also a constant fill).
	// Excluding the far depth (and near-zero/invalid depth) drops empty space while
	// KEEPING every real planar surface (slab, ground plane, cube faces) as a face.
	float MaxD = 0.f;
	for (int32 i = 0; i < NumPx; ++i) { MaxD = FMath::Max(MaxD, Depth[i]); }
	const float BgDepthThresh = MaxD * 0.999f;

	// Normalize normals + foreground mask.
	TArray<FVector3f> N;
	N.SetNumUninitialized(NumPx);
	TArray<uint8> Fg;
	Fg.SetNumUninitialized(NumPx);
	for (int32 i = 0; i < NumPx; ++i)
	{
		const float Len = Normal[i].Size();
		const bool bBackground = (Depth[i] >= BgDepthThresh) || (Depth[i] <= 1.f);
		if (Len > ForegroundNormalLen && !bBackground)
		{
			N[i] = Normal[i] / Len;
			Fg[i] = 1;
		}
		else
		{
			N[i] = FVector3f::ZeroVector;
			Fg[i] = 0;
		}
	}

	// Flood-fill into planar regions: grow across neighbors with continuous normal + depth.
	TArray<int32> Label;
	Label.Init(-1, NumPx);
	TArray<int32> RegionArea;
	TArray<int32> Stack;
	const int32 NX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	const int32 NY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	int32 NextLabel = 0;
	for (int32 s = 0; s < NumPx; ++s)
	{
		if (!Fg[s] || Label[s] != -1) { continue; }
		Label[s] = NextLabel;
		Stack.Reset();
		Stack.Add(s);
		int32 Count = 0;
		while (Stack.Num() > 0)
		{
			const int32 c = Stack.Pop(EAllowShrinking::No);
			++Count;
			const int32 cx = c % W;
			const int32 cy = c / W;
			const FVector3f Nc = N[c];
			const float Dc = Depth[c];
			for (int32 k = 0; k < 8; ++k)
			{
				const int32 nx = cx + NX[k];
				const int32 ny = cy + NY[k];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H) { continue; }
				const int32 ni = ny * W + nx;
				if (!Fg[ni] || Label[ni] != -1) { continue; }
				if (FVector3f::DotProduct(Nc, N[ni]) >= CosTol &&
					FMath::Abs(Depth[ni] - Dc) / FMath::Max(Dc, 1.0f) <= DepthJoinTol)
				{
					Label[ni] = NextLabel;
					Stack.Add(ni);
				}
			}
		}
		RegionArea.Add(Count);
		++NextLabel;
	}

	// Keep regions >= MinFaceArea, sorted by area desc -> face ids.
	TArray<int32> KeptLabels;
	for (int32 L = 0; L < NextLabel; ++L)
	{
		if (RegionArea[L] >= MinFaceArea) { KeptLabels.Add(L); }
	}
	KeptLabels.Sort([&](int32 A, int32 B) { return RegionArea[A] > RegionArea[B]; });

	TArray<int32> LabelToFace;
	LabelToFace.Init(-1, NextLabel);
	for (int32 f = 0; f < KeptLabels.Num(); ++f)
	{
		LabelToFace[KeptLabels[f]] = f;
	}
	const int32 NumFaces = KeptLabels.Num();

	// Per-face label mask used for contour tracing + visualization.
	TArray<int32> FaceLabel;
	FaceLabel.Init(-1, NumPx);
	for (int32 i = 0; i < NumPx; ++i)
	{
		if (Label[i] >= 0) { FaceLabel[i] = LabelToFace[Label[i]]; }
	}

	// Camera basis + projection for unprojection.
	const FTransform CamT = Camera->GetComponentTransform();
	const FVector Loc = CamT.GetLocation();
	const FVector Fwd = CamT.GetUnitAxis(EAxis::X);
	const FVector Rgt = CamT.GetUnitAxis(EAxis::Y);
	const FVector Up = CamT.GetUnitAxis(EAxis::Z);
	const float TanX = FMath::Tan(FMath::DegreesToRadians(Camera->FieldOfView * 0.5f));
	const float TanY = TanX * (static_cast<float>(H) / static_cast<float>(W));
	const bool bOrtho = bCaptureOrthographic;
	const double OrthoW = CaptureOrthoWidth;

	auto CamRayDir = [&](double px, double py) -> FVector
	{
		const double ndcX = 2.0 * ((px + 0.5) / W) - 1.0;
		const double ndcY = 1.0 - 2.0 * ((py + 0.5) / H);
		return Fwd + Rgt * (ndcX * TanX) + Up * (ndcY * TanY); // forward coeff == 1
	};
	auto OrthoRayOrigin = [&](double px, double py) -> FVector
	{
		const double ndcX = 2.0 * ((px + 0.5) / W) - 1.0;
		const double ndcY = 1.0 - 2.0 * ((py + 0.5) / H);
		return Loc + Rgt * (ndcX * OrthoW * 0.5) + Up * (ndcY * OrthoW * 0.5 * (static_cast<double>(H) / W));
	};
	auto Unproject = [&](int32 px, int32 py, float depth) -> FVector
	{
		if (bOrtho)
		{
			return OrthoRayOrigin(px, py) + Fwd * depth;
		}
		// SceneDepth treated as planar (view-space Z); forward coeff of the ray is 1.
		return Loc + CamRayDir(px, py) * depth;
	};

	// Accumulate plane (mean 3D point + mean normal) and 2D centroid per face.
	TArray<FVector> SumPt; SumPt.Init(FVector::ZeroVector, NumFaces);
	TArray<FVector> SumN; SumN.Init(FVector::ZeroVector, NumFaces);
	TArray<int64> Cnt; Cnt.Init(0, NumFaces);
	TArray<FVector2D> SumPx; SumPx.Init(FVector2D::ZeroVector, NumFaces);
	TArray<int32> FirstPixel; FirstPixel.Init(-1, NumFaces);
	for (int32 i = 0; i < NumPx; ++i)
	{
		const int32 f = FaceLabel[i];
		if (f < 0) { continue; }
		const int32 px = i % W;
		const int32 py = i / W;
		SumPt[f] += Unproject(px, py, Depth[i]);
		SumN[f] += FVector(N[i].X, N[i].Y, N[i].Z);
		SumPx[f] += FVector2D(px, py);
		Cnt[f] += 1;
		if (FirstPixel[f] < 0) { FirstPixel[f] = i; }
	}

	// Color palette for the visualization + JSON legend.
	static const FColor Palette[] = {
		FColor(220, 50, 47), FColor(38, 139, 210), FColor(133, 153, 0), FColor(181, 137, 0),
		FColor(211, 54, 130), FColor(42, 161, 152), FColor(203, 75, 22), FColor(108, 113, 196),
		FColor(0, 150, 136), FColor(156, 39, 176), FColor(255, 152, 0), FColor(96, 125, 139)
	};
	const int32 NumPalette = UE_ARRAY_COUNT(Palette);

	// Build JSON + faces visualization.
	TArray<FColor> Out;
	Out.Init(FColor(255, 255, 255, 255), NumPx);

	FString Json;
	Json += TEXT("{\n");
	Json += FString::Printf(TEXT("  \"image\": { \"w\": %d, \"h\": %d },\n"), W, H);
	Json += FString::Printf(TEXT("  \"num_faces\": %d,\n"), NumFaces);
	Json += TEXT("  \"faces\": [\n");

	for (int32 f = 0; f < NumFaces; ++f)
	{
		const FColor Col = Palette[f % NumPalette];

		// Paint region pixels.
		for (int32 i = 0; i < NumPx; ++i)
		{
			if (FaceLabel[i] == f) { Out[i] = Col; }
		}

		const double InvCnt = Cnt[f] > 0 ? 1.0 / static_cast<double>(Cnt[f]) : 0.0;
		const FVector PlanePt = SumPt[f] * InvCnt;
		FVector PlaneN = SumN[f];
		PlaneN = PlaneN.GetSafeNormal();
		const FVector2D Centroid2D = SumPx[f] * InvCnt;

		// Contour -> RDP corners (2D). Anchor RDP at the contour point farthest from
		// the centroid so a true corner is an endpoint of the open polyline.
		TArray<FIntPoint> Contour;
		TraceContour(FaceLabel, W, H, f, FirstPixel[f], Contour);

		TArray<FVector2D> Corners2D;
		if (Contour.Num() >= 3)
		{
			TArray<FVector2D> CP;
			CP.Reserve(Contour.Num());
			for (const FIntPoint& P : Contour) { CP.Add(FVector2D(P.X, P.Y)); }

			int32 Anchor = 0;
			double BestD = -1.0;
			for (int32 i = 0; i < CP.Num(); ++i)
			{
				const double D = FVector2D::DistSquared(CP[i], Centroid2D);
				if (D > BestD) { BestD = D; Anchor = i; }
			}
			TArray<FVector2D> Rot;
			Rot.Reserve(CP.Num() + 1);
			for (int32 i = 0; i < CP.Num(); ++i) { Rot.Add(CP[(Anchor + i) % CP.Num()]); }
			const FVector2D ClosePt = Rot[0];
			Rot.Add(ClosePt); // close

			TArray<int32> Keep;
			Keep.Add(0);
			RDP(Rot, 0, Rot.Num() - 1, RdpEps, Keep);
			// drop the duplicated closing point's index if present
			for (int32 idx : Keep)
			{
				if (idx >= 0 && idx < Rot.Num() - 1) { Corners2D.Add(Rot[idx]); }
			}
		}

		// Corner 3D = camera ray through the corner intersected with the fitted plane.
		TArray<FVector> Corners3D;
		Corners3D.Reserve(Corners2D.Num());
		for (const FVector2D& C2 : Corners2D)
		{
			const FVector RayOrigin = bOrtho ? OrthoRayOrigin(C2.X, C2.Y) : Loc;
			const FVector Dir = bOrtho ? Fwd : CamRayDir(C2.X, C2.Y).GetSafeNormal();
			const double Denom = FVector::DotProduct(Dir, PlaneN);
			FVector Hit = PlanePt;
			if (FMath::Abs(Denom) > 1e-5)
			{
				const double T = FVector::DotProduct(PlanePt - RayOrigin, PlaneN) / Denom;
				Hit = RayOrigin + Dir * T;
			}
			Corners3D.Add(Hit);
		}

		// JSON entry.
		Json += TEXT("    {\n");
		Json += FString::Printf(TEXT("      \"id\": %d,\n"), f);
		Json += FString::Printf(TEXT("      \"color_rgb\": [%d, %d, %d],\n"), Col.R, Col.G, Col.B);
		Json += FString::Printf(TEXT("      \"area_px\": %lld,\n"), static_cast<long long>(Cnt[f]));
		Json += FString::Printf(TEXT("      \"normal_world\": [%.5f, %.5f, %.5f],\n"), PlaneN.X, PlaneN.Y, PlaneN.Z);
		Json += FString::Printf(TEXT("      \"plane_point\": [%.3f, %.3f, %.3f],\n"), PlanePt.X, PlanePt.Y, PlanePt.Z);
		Json += FString::Printf(TEXT("      \"centroid_2d\": [%.2f, %.2f],\n"), Centroid2D.X, Centroid2D.Y);

		Json += TEXT("      \"key_points_2d\": [");
		for (int32 k = 0; k < Corners2D.Num(); ++k)
		{
			Json += FString::Printf(TEXT("%s[%.2f, %.2f]"), (k == 0 ? TEXT("") : TEXT(", ")), Corners2D[k].X, Corners2D[k].Y);
		}
		Json += TEXT("],\n");

		Json += TEXT("      \"key_points_3d\": [");
		for (int32 k = 0; k < Corners3D.Num(); ++k)
		{
			Json += FString::Printf(TEXT("%s[%.3f, %.3f, %.3f]"), (k == 0 ? TEXT("") : TEXT(", ")), Corners3D[k].X, Corners3D[k].Y, Corners3D[k].Z);
		}
		Json += TEXT("]\n");

		Json += FString::Printf(TEXT("    }%s\n"), (f + 1 < NumFaces ? TEXT(",") : TEXT("")));
	}
	Json += TEXT("  ]\n}\n");

	// Save faces PNG (FColor is BGRA in memory).
	IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> IW = IWM.CreateImageWrapper(EImageFormat::PNG);
	if (IW.IsValid())
	{
		IW->SetRaw(Out.GetData(), Out.Num() * sizeof(FColor), W, H, ERGBFormat::BGRA, 8);
		const TArray64<uint8>& Compressed = IW->GetCompressed();
		FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Compressed.GetData(), static_cast<int32>(Compressed.Num())), *FacesPngPath);
	}
	FFileHelper::SaveStringToFile(Json, *FacesJsonPath);

	UE_LOG(LogTemp, Log, TEXT("CaptureFromLZ: %d planar face(s) -> %s"), NumFaces, *FacesJsonPath);
}

namespace FromLZActorMaterialId
{
	struct FProjectedVertex
	{
		FVector2D Pixel = FVector2D::ZeroVector;
		double Depth = 0.0;
		bool bValid = false;
	};

	static FColor EncodeIdColor(int32 Id)
	{
		return FColor(
			uint8((Id >> 16) & 0xff),
			uint8((Id >> 8) & 0xff),
			uint8(Id & 0xff),
			255);
	}

	static TArray<TSharedPtr<FJsonValue>> JsonColorArray(const FColor& Color)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Add(MakeShared<FJsonValueNumber>(Color.R));
		Values.Add(MakeShared<FJsonValueNumber>(Color.G));
		Values.Add(MakeShared<FJsonValueNumber>(Color.B));
		return Values;
	}

	static bool ProjectWorldToCapturePixel(
		const FVector& WorldPosition,
		const FTransform& CameraTransform,
		int32 W,
		int32 H,
		double CaptureOrthoWidth,
		FProjectedVertex& Out)
	{
		Out = FProjectedVertex();
		if (W <= 0 || H <= 0 || CaptureOrthoWidth <= 1e-6)
		{
			return false;
		}

		const FVector Loc = CameraTransform.GetLocation();
		const FVector Fwd = CameraTransform.GetUnitAxis(EAxis::X);
		const FVector Rgt = CameraTransform.GetUnitAxis(EAxis::Y);
		const FVector Up = CameraTransform.GetUnitAxis(EAxis::Z);
		const FVector Delta = WorldPosition - Loc;
		const double Depth = FVector::DotProduct(Delta, Fwd);
		if (!FMath::IsFinite(Depth) || Depth <= 0.0)
		{
			return false;
		}

		const double HalfW = CaptureOrthoWidth * 0.5;
		const double HalfH = HalfW * (double(H) / double(W));
		const double NdcX = FVector::DotProduct(Delta, Rgt) / HalfW;
		const double NdcY = FVector::DotProduct(Delta, Up) / HalfH;
		Out.Pixel.X = ((NdcX + 1.0) * 0.5 * double(W)) - 0.5;
		Out.Pixel.Y = ((1.0 - NdcY) * 0.5 * double(H)) - 0.5;
		Out.Depth = Depth;
		Out.bValid = FMath::IsFinite(Out.Pixel.X) && FMath::IsFinite(Out.Pixel.Y);
		return Out.bValid;
	}

	static bool Barycentric2D(
		const FVector2D& P,
		const FVector2D& A,
		const FVector2D& B,
		const FVector2D& C,
		double& OutA,
		double& OutB,
		double& OutC)
	{
		const double Denom =
			(double(B.Y) - double(C.Y)) * (double(A.X) - double(C.X)) +
			(double(C.X) - double(B.X)) * (double(A.Y) - double(C.Y));
		if (FMath::Abs(Denom) < 1e-8)
		{
			return false;
		}

		OutA = ((double(B.Y) - double(C.Y)) * (double(P.X) - double(C.X)) +
			(double(C.X) - double(B.X)) * (double(P.Y) - double(C.Y))) / Denom;
		OutB = ((double(C.Y) - double(A.Y)) * (double(P.X) - double(C.X)) +
			(double(A.X) - double(C.X)) * (double(P.Y) - double(C.Y))) / Denom;
		OutC = 1.0 - OutA - OutB;
		return FMath::IsFinite(OutA) && FMath::IsFinite(OutB) && FMath::IsFinite(OutC);
	}

	static int32 FindOrAddEntry(
		UPrimitiveComponent* Component,
		int32 MaterialSlot,
		TMap<FString, int32>& EntryIdByKey,
		TArray<TSharedPtr<FJsonValue>>& Entries)
	{
		if (!Component || MaterialSlot < 0)
		{
			return 0;
		}

		const FString Key = FString::Printf(TEXT("%s|%d"), *Component->GetPathName(), MaterialSlot);
		if (const int32* Existing = EntryIdByKey.Find(Key))
		{
			return *Existing;
		}

		const int32 Id = Entries.Num() + 1;
		if (Id > 0x00ffffff)
		{
			return 0;
		}

		AActor* Owner = Component->GetOwner();
		UMaterialInterface* Material = Component->GetMaterial(MaterialSlot);
		const FColor Color = EncodeIdColor(Id);
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("id"), Id);
		Entry->SetArrayField(TEXT("color_rgb"), JsonColorArray(Color));
		Entry->SetStringField(TEXT("actor_name"), Owner ? Owner->GetName() : FString());
		Entry->SetStringField(TEXT("actor_path"), Owner ? Owner->GetPathName() : FString());
#if WITH_EDITOR
		Entry->SetStringField(TEXT("actor_label"), Owner ? Owner->GetActorLabel() : FString());
#endif
		Entry->SetStringField(TEXT("component_name"), Component->GetName());
		Entry->SetStringField(TEXT("component_path"), Component->GetPathName());
		Entry->SetStringField(TEXT("component_type"), Component->GetClass() ? Component->GetClass()->GetName() : FString());
		Entry->SetNumberField(TEXT("material_slot"), MaterialSlot);
		Entry->SetStringField(TEXT("material_name"), GetNameSafe(Material));
		Entry->SetStringField(TEXT("material_path"), Material ? Material->GetPathName() : FString());
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
		EntryIdByKey.Add(Key, Id);
		return Id;
	}

	static void RasterizeTriangle(
		const FProjectedVertex& A,
		const FProjectedVertex& B,
		const FProjectedVertex& C,
		int32 Id,
		int32 W,
		int32 H,
		TArray<double>& ZBuffer,
		TArray<FColor>& OutPixels)
	{
		if (!A.bValid || !B.bValid || !C.bValid || Id <= 0)
		{
			return;
		}

		const double MinX = FMath::Min3(A.Pixel.X, B.Pixel.X, C.Pixel.X);
		const double MaxX = FMath::Max3(A.Pixel.X, B.Pixel.X, C.Pixel.X);
		const double MinY = FMath::Min3(A.Pixel.Y, B.Pixel.Y, C.Pixel.Y);
		const double MaxY = FMath::Max3(A.Pixel.Y, B.Pixel.Y, C.Pixel.Y);
		int32 X0 = FMath::Clamp(FMath::FloorToInt(MinX), 0, W - 1);
		int32 X1 = FMath::Clamp(FMath::CeilToInt(MaxX), 0, W - 1);
		int32 Y0 = FMath::Clamp(FMath::FloorToInt(MinY), 0, H - 1);
		int32 Y1 = FMath::Clamp(FMath::CeilToInt(MaxY), 0, H - 1);
		if (X1 < X0 || Y1 < Y0)
		{
			return;
		}

		const FColor Color = EncodeIdColor(Id);
		for (int32 Y = Y0; Y <= Y1; ++Y)
		{
			for (int32 X = X0; X <= X1; ++X)
			{
				double Wa = 0.0;
				double Wb = 0.0;
				double Wc = 0.0;
				const FVector2D P(static_cast<double>(X), static_cast<double>(Y));
				if (!Barycentric2D(P, A.Pixel, B.Pixel, C.Pixel, Wa, Wb, Wc))
				{
					continue;
				}

				constexpr double EdgeTolerance = -1e-4;
				if (Wa < EdgeTolerance || Wb < EdgeTolerance || Wc < EdgeTolerance)
				{
					continue;
				}

				const double Depth = Wa * A.Depth + Wb * B.Depth + Wc * C.Depth;
				const int32 PixelIndex = Y * W + X;
				if (Depth > 0.0 && Depth < ZBuffer[PixelIndex])
				{
					ZBuffer[PixelIndex] = Depth;
					OutPixels[PixelIndex] = Color;
				}
			}
		}
	}

	static void RasterizeStaticMeshComponent(
		UStaticMeshComponent* Component,
		const FTransform& CameraTransform,
		int32 W,
		int32 H,
		TMap<FString, int32>& EntryIdByKey,
		TArray<TSharedPtr<FJsonValue>>& Entries,
		TArray<double>& ZBuffer,
		TArray<FColor>& OutPixels)
	{
		if (!Component || !Component->IsRegistered() || !Component->IsVisible())
		{
			return;
		}

		UStaticMesh* StaticMesh = Component->GetStaticMesh();
		if (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.Num() == 0)
		{
			return;
		}

		const FStaticMeshLODResources& LOD = StaticMesh->GetRenderData()->LODResources[0];
		const FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
		const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
		const int32 NumVertices = PositionBuffer.GetNumVertices();
		if (NumVertices <= 0 || Indices.Num() < 3)
		{
			return;
		}

		TArray<FProjectedVertex> Projected;
		Projected.SetNum(NumVertices);
		const FTransform& ComponentTransform = Component->GetComponentTransform();
		for (int32 VertexIndex = 0; VertexIndex < NumVertices; ++VertexIndex)
		{
			const FVector3f Local = PositionBuffer.VertexPosition(VertexIndex);
			const FVector World = ComponentTransform.TransformPosition(FVector(Local.X, Local.Y, Local.Z));
			ProjectWorldToCapturePixel(World, CameraTransform, W, H, FromLZCaptureOrthoWidth, Projected[VertexIndex]);
		}

		for (const FStaticMeshSection& Section : LOD.Sections)
		{
			const int32 MaterialSlot = int32(Section.MaterialIndex);
			const int32 Id = FindOrAddEntry(Component, MaterialSlot, EntryIdByKey, Entries);
			for (uint32 TriIndex = 0; TriIndex < Section.NumTriangles; ++TriIndex)
			{
				const uint32 IndexBase = Section.FirstIndex + TriIndex * 3;
				if (IndexBase + 2 >= uint32(Indices.Num()))
				{
					continue;
				}
				const int32 IA = int32(Indices[IndexBase]);
				const int32 IB = int32(Indices[IndexBase + 1]);
				const int32 IC = int32(Indices[IndexBase + 2]);
				if (!Projected.IsValidIndex(IA) || !Projected.IsValidIndex(IB) || !Projected.IsValidIndex(IC))
				{
					continue;
				}
				RasterizeTriangle(Projected[IA], Projected[IB], Projected[IC], Id, W, H, ZBuffer, OutPixels);
			}
		}
	}

	static void RasterizeProceduralMeshComponent(
		UProceduralMeshComponent* Component,
		const FTransform& CameraTransform,
		int32 W,
		int32 H,
		TMap<FString, int32>& EntryIdByKey,
		TArray<TSharedPtr<FJsonValue>>& Entries,
		TArray<double>& ZBuffer,
		TArray<FColor>& OutPixels)
	{
		if (!Component || !Component->IsRegistered() || !Component->IsVisible())
		{
			return;
		}

		const FTransform& ComponentTransform = Component->GetComponentTransform();
		for (int32 SectionIndex = 0; SectionIndex < Component->GetNumSections(); ++SectionIndex)
		{
			if (!Component->IsMeshSectionVisible(SectionIndex))
			{
				continue;
			}

			const FProcMeshSection* Section = Component->GetProcMeshSection(SectionIndex);
			if (!Section || Section->ProcVertexBuffer.Num() < 3 || Section->ProcIndexBuffer.Num() < 3)
			{
				continue;
			}

			TArray<FProjectedVertex> Projected;
			Projected.SetNum(Section->ProcVertexBuffer.Num());
			for (int32 VertexIndex = 0; VertexIndex < Section->ProcVertexBuffer.Num(); ++VertexIndex)
			{
				const FVector World = ComponentTransform.TransformPosition(Section->ProcVertexBuffer[VertexIndex].Position);
				ProjectWorldToCapturePixel(World, CameraTransform, W, H, FromLZCaptureOrthoWidth, Projected[VertexIndex]);
			}

			const int32 Id = FindOrAddEntry(Component, SectionIndex, EntryIdByKey, Entries);
			for (int32 Index = 0; Index + 2 < Section->ProcIndexBuffer.Num(); Index += 3)
			{
				const int32 IA = Section->ProcIndexBuffer[Index];
				const int32 IB = Section->ProcIndexBuffer[Index + 1];
				const int32 IC = Section->ProcIndexBuffer[Index + 2];
				if (!Projected.IsValidIndex(IA) || !Projected.IsValidIndex(IB) || !Projected.IsValidIndex(IC))
				{
					continue;
				}
				RasterizeTriangle(Projected[IA], Projected[IB], Projected[IC], Id, W, H, ZBuffer, OutPixels);
			}
		}
	}

	static bool SaveActorMaterialIdBuffer(
		UWorld* World,
		const UCameraComponent* Camera,
		int32 W,
		int32 H,
		const FString& PngPath,
		const FString& JsonPath)
	{
		if (!World || !Camera || W <= 0 || H <= 0)
		{
			return false;
		}

		TArray<FColor> Pixels;
		Pixels.Init(FColor(0, 0, 0, 0), W * H);
		TArray<double> ZBuffer;
		ZBuffer.Init(TNumericLimits<double>::Max(), W * H);
		TMap<FString, int32> EntryIdByKey;
		TArray<TSharedPtr<FJsonValue>> Entries;
		const FTransform CameraTransform = Camera->GetComponentTransform();

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsHidden())
			{
				continue;
			}

			TArray<UStaticMeshComponent*> StaticComponents;
			Actor->GetComponents<UStaticMeshComponent>(StaticComponents);
			for (UStaticMeshComponent* Component : StaticComponents)
			{
				RasterizeStaticMeshComponent(Component, CameraTransform, W, H, EntryIdByKey, Entries, ZBuffer, Pixels);
			}

			TArray<UProceduralMeshComponent*> ProceduralComponents;
			Actor->GetComponents<UProceduralMeshComponent>(ProceduralComponents);
			for (UProceduralMeshComponent* Component : ProceduralComponents)
			{
				RasterizeProceduralMeshComponent(Component, CameraTransform, W, H, EntryIdByKey, Entries, ZBuffer, Pixels);
			}
		}

		IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> IW = IWM.CreateImageWrapper(EImageFormat::PNG);
		if (!IW.IsValid())
		{
			return false;
		}
		IW->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), W, H, ERGBFormat::BGRA, 8);
		const TArray64<uint8>& Compressed = IW->GetCompressed();
		const bool bSavedPng = FFileHelper::SaveArrayToFile(
			TArrayView<const uint8>(Compressed.GetData(), static_cast<int32>(Compressed.Num())),
			*PngPath);

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("version"), 1);
		TSharedRef<FJsonObject> ImageObject = MakeShared<FJsonObject>();
		ImageObject->SetNumberField(TEXT("w"), W);
		ImageObject->SetNumberField(TEXT("h"), H);
		Root->SetObjectField(TEXT("image"), ImageObject);
		Root->SetStringField(TEXT("encoding"), TEXT("rgb24_id_background_zero"));
		Root->SetStringField(TEXT("projection_mode"), TEXT("Orthographic"));
		Root->SetNumberField(TEXT("ortho_width"), FromLZCaptureOrthoWidth);
		Root->SetArrayField(TEXT("entries"), Entries);

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		const bool bSerialized = FJsonSerializer::Serialize(Root, Writer);
		const bool bSavedJson = bSerialized && FFileHelper::SaveStringToFile(JsonText, *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		if (bSavedPng && bSavedJson)
		{
			UE_LOG(LogTemp, Log, TEXT("CaptureFromLZ: actor/material id buffer entries=%d -> %s"), Entries.Num(), *JsonPath);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ: failed to save actor/material id buffer png=%d json=%d (%s / %s)."), bSavedPng ? 1 : 0, bSavedJson ? 1 : 0, *PngPath, *JsonPath);
		}
		return bSavedPng && bSavedJson;
	}
}

// Renders scene depth and world-normal to off-screen render targets on the GPU,
// then runs a CPU Sobel edge-detection pass to produce a white background with
// black contour/crease lines (depth discontinuity = silhouette/occlusion edges,
// normal discontinuity = surface creases). Occlusion is handled naturally because
// the detection works in screen space on the rendered buffers.
static bool CaptureLineArtPng(const APawn* Pawn, const UCameraComponent* Camera, const FString& OutputPath)
{
	UWorld* World = Pawn->GetWorld();
	if (!World)
	{
		return false;
	}

	FIntPoint Size(1920, 1080);
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		Size = GEngine->GameViewport->Viewport->GetSizeXY();
	}
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return false;
	}

	auto MakeRenderTarget = [&](ETextureRenderTargetFormat Format) -> UTextureRenderTarget2D*
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		RT->RenderTargetFormat = Format;
		RT->ClearColor = FLinearColor::Black;
		RT->bAutoGenerateMips = false;
		RT->InitAutoFormat(Size.X, Size.Y);
		RT->UpdateResourceImmediate(true);
		RT->AddToRoot();
		return RT;
	};

	// 16-bit float so normals can store negative components and depth keeps range.
	UTextureRenderTarget2D* DepthRT = MakeRenderTarget(RTF_RGBA16f);
	UTextureRenderTarget2D* NormalRT = MakeRenderTarget(RTF_RGBA16f);

	USceneCaptureComponent2D* SCC = NewObject<USceneCaptureComponent2D>(const_cast<APawn*>(Pawn), NAME_None, RF_Transient);
	SCC->bCaptureEveryFrame = false;
	SCC->bCaptureOnMovement = false;
	SCC->bAlwaysPersistRenderingState = true;
	SCC->SetWorldTransform(Camera->GetComponentTransform());
	SCC->ProjectionType = ECameraProjectionMode::Orthographic;
	SCC->FOVAngle = Camera->FieldOfView;
	SCC->OrthoWidth = FromLZCaptureOrthoWidth;
	SCC->RegisterComponentWithWorld(World);

	// Restrict the capture to actors whose name starts with "Cube" so that the
	// ground plane, sky and other background geometry never enter the line-art
	// pass. The Outliner label (e.g. "Cube") usually differs from the internal
	// object name, so match against the editor label first and fall back to the
	// internal name. If nothing matches, render the whole scene.
	// Plain local (NOT static): a function-local "static" FString is not
	// re-initialized by Live Coding hot-reload and can end up empty.
	const FString CaptureNamePrefix(TEXT("Cube"));
	int32 ShowOnlyCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		bool bMatch = Actor->GetName().StartsWith(CaptureNamePrefix);
#if WITH_EDITOR
		bMatch = bMatch || Actor->GetActorLabel().StartsWith(CaptureNamePrefix);
#endif
		if (bMatch)
		{
			SCC->ShowOnlyActors.Add(Actor);
			++ShowOnlyCount;
		}
	}

	if (ShowOnlyCount > 0)
	{
		SCC->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		UE_LOG(LogTemp, Log, TEXT("CaptureFromLZ: line-art restricted to %d actor(s) named '%s*'."), ShowOnlyCount, *CaptureNamePrefix);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ: no actor named '%s*'; capturing the whole scene (ground/sky may appear)."), *CaptureNamePrefix);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
#if WITH_EDITOR
			UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ: candidate actor label='%s' name='%s'"), *It->GetActorLabel(), *It->GetName());
#else
			UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ: candidate actor name='%s'"), *It->GetName());
#endif
		}
	}

	// Captures linear scene depth + world normal with the SCC's current config and
	// reads them back to CPU arrays. Reused for the (Cube-only) line art and the
	// (whole-scene) face segmentation.
	auto CaptureDepthNormal = [&](TArray<float>& OutDepth, TArray<FVector3f>& OutNormal) -> bool
	{
		SCC->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
		SCC->TextureTarget = DepthRT;
		SCC->CaptureScene();

		SCC->CaptureSource = ESceneCaptureSource::SCS_Normal;
		SCC->TextureTarget = NormalRT;
		SCC->CaptureScene();

		FlushRenderingCommands();

		TArray<FFloat16Color> DepthPixels;
		TArray<FFloat16Color> NormalPixels;
		FTextureRenderTargetResource* DRes = DepthRT->GameThread_GetRenderTargetResource();
		FTextureRenderTargetResource* NRes = NormalRT->GameThread_GetRenderTargetResource();
		if (!(DRes && DRes->ReadFloat16Pixels(DepthPixels) &&
			  NRes && NRes->ReadFloat16Pixels(NormalPixels) &&
			  DepthPixels.Num() == Size.X * Size.Y &&
			  NormalPixels.Num() == Size.X * Size.Y))
		{
			return false;
		}
		const int32 Num = Size.X * Size.Y;
		OutDepth.SetNumUninitialized(Num);
		OutNormal.SetNumUninitialized(Num);
		for (int32 i = 0; i < Num; ++i)
		{
			OutDepth[i] = DepthPixels[i].R.GetFloat();
			OutNormal[i] = FVector3f(NormalPixels[i].R.GetFloat(), NormalPixels[i].G.GetFloat(), NormalPixels[i].B.GetFloat());
		}
		return true;
	};

	// Pass set 1: Cube-restricted buffers -> line art.
	bool bSaved = false;
	TArray<float> Depth;
	TArray<FVector3f> Normal;
	const bool bReadOk = CaptureDepthNormal(Depth, Normal);

	if (bReadOk)
	{
		const int32 W = Size.X;
		const int32 H = Size.Y;

		// Tunable edge-detection thresholds.
		const float DepthRelThreshold = 0.03f; // relative depth gradient (gradient / depth)
		const float NormalThreshold = 0.6f;    // normal gradient magnitude

		auto SampleDepth = [&](int32 x, int32 y) -> float
		{
			x = FMath::Clamp(x, 0, W - 1);
			y = FMath::Clamp(y, 0, H - 1);
			return Depth[y * W + x];
		};
		auto SampleNormal = [&](int32 x, int32 y) -> FVector3f
		{
			x = FMath::Clamp(x, 0, W - 1);
			y = FMath::Clamp(y, 0, H - 1);
			return Normal[y * W + x];
		};

		TArray<FColor> Out;
		Out.SetNumUninitialized(W * H);

		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				const float Dc = SampleDepth(x, y);

				// Slant-invariant depth discontinuity: compare the center depth against
				// the linear prediction from opposite neighbors (discrete 2nd derivative).
				// A planar surface is locally linear in depth even when viewed edge-on, so
				// this stays ~0 there; only true depth jumps (silhouettes/occlusions) and
				// creases fire. This avoids filling grazing faces solid black.
				const float DevX = FMath::Abs(2.f * Dc - SampleDepth(x - 1, y) - SampleDepth(x + 1, y));
				const float DevY = FMath::Abs(2.f * Dc - SampleDepth(x, y - 1) - SampleDepth(x, y + 1));
				const float DevD1 = FMath::Abs(2.f * Dc - SampleDepth(x - 1, y - 1) - SampleDepth(x + 1, y + 1));
				const float DevD2 = FMath::Abs(2.f * Dc - SampleDepth(x + 1, y - 1) - SampleDepth(x - 1, y + 1));
				const float DepthDev = FMath::Max(FMath::Max(DevX, DevY), FMath::Max(DevD1, DevD2));
				const bool bDepthEdge = (DepthDev / FMath::Max(Dc, 1.f)) > DepthRelThreshold;

				const FVector3f N00 = SampleNormal(x - 1, y - 1);
				const FVector3f N10 = SampleNormal(x, y - 1);
				const FVector3f N20 = SampleNormal(x + 1, y - 1);
				const FVector3f N01 = SampleNormal(x - 1, y);
				const FVector3f N21 = SampleNormal(x + 1, y);
				const FVector3f N02 = SampleNormal(x - 1, y + 1);
				const FVector3f N12 = SampleNormal(x, y + 1);
				const FVector3f N22 = SampleNormal(x + 1, y + 1);

				const FVector3f Ngx = -N00 - 2.f * N01 - N02 + N20 + 2.f * N21 + N22;
				const FVector3f Ngy = -N00 - 2.f * N10 - N20 + N02 + 2.f * N12 + N22;
				const float NormalGrad = FMath::Sqrt(Ngx.SizeSquared() + Ngy.SizeSquared());
				const bool bNormalEdge = NormalGrad > NormalThreshold;

				Out[y * W + x] = (bDepthEdge || bNormalEdge) ? FColor(0, 0, 0, 255) : FColor(255, 255, 255, 255);
			}
		}

		IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> IW = IWM.CreateImageWrapper(EImageFormat::PNG);
		if (IW.IsValid())
		{
			// FColor memory layout is B,G,R,A — use ERGBFormat::BGRA
			IW->SetRaw(Out.GetData(), Out.Num() * sizeof(FColor), W, H, ERGBFormat::BGRA, 8);
			const TArray64<uint8>& Compressed = IW->GetCompressed();
			bSaved = FFileHelper::SaveArrayToFile(
				TArrayView<const uint8>(Compressed.GetData(), static_cast<int32>(Compressed.Num())),
				*OutputPath
			);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ: failed to read depth/normal render targets."));
	}

	// Pass set 2: whole-scene buffers -> planar face segmentation. Unlike the line
	// art, faces include every object in the scene (ground plane, etc.), so we drop
	// the Cube-only show list and render all primitives.
	{
		SCC->ShowOnlyActors.Empty();
		SCC->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_LegacySceneCapture;
		TArray<float> FaceDepth;
		TArray<FVector3f> FaceNormal;
		if (CaptureDepthNormal(FaceDepth, FaceNormal))
		{
			const FString FacesBase = FPaths::Combine(FPaths::GetPath(OutputPath), FPaths::GetBaseFilename(OutputPath));
			SaveNormalFaces(
				FaceDepth, FaceNormal, Size.X, Size.Y, Camera,
				/*bCaptureOrthographic*/ true, FromLZCaptureOrthoWidth,
				FacesBase + TEXT("_faces.png"), FacesBase + TEXT("_faces.json"));
			FromLZActorMaterialId::SaveActorMaterialIdBuffer(
				World,
				Camera,
				Size.X,
				Size.Y,
				FacesBase + TEXT("_actor_material_id.png"),
				FacesBase + TEXT("_actor_material_id.json"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ: failed to read whole-scene depth/normal for faces."));
		}
	}

	SCC->UnregisterComponent();
	SCC->DestroyComponent();
	DepthRT->RemoveFromRoot();
	NormalRT->RemoveFromRoot();

	return bSaved;
}

bool FFromLZCaptureUtils::CaptureFromPawn(const APawn* Pawn)
{
	if (!Pawn)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ failed: controlled pawn is null."));
		return false;
	}

	UCameraComponent* CameraComponent = FindFromLZCamera(Pawn);
	USpringArmComponent* CameraBoom = FindCameraBoom(Pawn);

	if (!CameraComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ failed: FromLZ camera component was not found on pawn %s."), *Pawn->GetName());
		return false;
	}

	const FString CaptureDirectory = FPaths::ProjectSavedDir() / TEXT("FromLZCaptures");
	IFileManager::Get().MakeDirectory(*CaptureDirectory, true);

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString BaseFilename = FString::Printf(TEXT("FromLZ_%s"), *Timestamp);
	const FString JsonPath = CaptureDirectory / (BaseFilename + TEXT(".json"));
	const FString PngPath = CaptureDirectory / (BaseFilename + TEXT(".png"));
	const FString ActorMaterialPngPath = CaptureDirectory / (BaseFilename + TEXT("_actor_material_id.png"));
	const FString ActorMaterialJsonPath = CaptureDirectory / (BaseFilename + TEXT("_actor_material_id.json"));

	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("capture_timestamp"), Timestamp);
	RootObject->SetStringField(TEXT("json_path"), JsonPath);
	RootObject->SetStringField(TEXT("png_path"), PngPath);
	RootObject->SetStringField(TEXT("actor_material_png_path"), ActorMaterialPngPath);
	RootObject->SetStringField(TEXT("actor_material_json_path"), ActorMaterialJsonPath);
	RootObject->SetObjectField(TEXT("pawn"), SerializeObjectProperties(Pawn));
	RootObject->SetObjectField(TEXT("pawn_transform"), SerializeTransform(Pawn->GetActorTransform()));
	RootObject->SetObjectField(TEXT("camera_component"), SerializeObjectProperties(CameraComponent));
	RootObject->SetObjectField(TEXT("camera_component_transform"), SerializeTransform(CameraComponent->GetComponentTransform()));

	if (CameraBoom)
	{
		RootObject->SetObjectField(TEXT("camera_boom"), SerializeObjectProperties(CameraBoom));
		RootObject->SetObjectField(TEXT("camera_boom_transform"), SerializeTransform(CameraBoom->GetComponentTransform()));
	}

	TSharedRef<FJsonObject> ViewObject = MakeShared<FJsonObject>();
	ViewObject->SetNumberField(TEXT("fov"), CameraComponent->FieldOfView);
	ViewObject->SetNumberField(TEXT("aspect_ratio"), CameraComponent->AspectRatio);
	ViewObject->SetBoolField(TEXT("constrain_aspect_ratio"), CameraComponent->bConstrainAspectRatio);
	ViewObject->SetStringField(TEXT("projection_mode"), StaticEnum<ECameraProjectionMode::Type>()->GetValueAsString(ECameraProjectionMode::Orthographic));
	ViewObject->SetNumberField(TEXT("ortho_width"), FromLZCaptureOrthoWidth);
	ViewObject->SetNumberField(TEXT("near_clip_plane"), CameraComponent->OrthoNearClipPlane);
	ViewObject->SetNumberField(TEXT("far_clip_plane"), CameraComponent->OrthoFarClipPlane);
	RootObject->SetObjectField(TEXT("camera_view"), ViewObject);

	if (!SaveJsonToFile(RootObject, JsonPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ failed: could not write json file %s"), *JsonPath);
		return false;
	}

	if (CaptureLineArtPng(Pawn, CameraComponent, PngPath))
	{
		UE_LOG(LogTemp, Log, TEXT("CaptureFromLZ saved json to %s and line-art png to %s"), *JsonPath, *PngPath);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ saved json to %s but line-art png failed: %s"), *JsonPath, *PngPath);
	}
	return true;
}

bool FFromLZCaptureUtils::CaptureFromWorld(const UWorld* World)
{
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ failed: world is null."));
		return false;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		if (const APlayerController* PlayerController = It->Get())
		{
			if (const APawn* Pawn = PlayerController->GetPawn())
			{
				UE_LOG(LogTemp, Log, TEXT("CaptureFromLZ attempting capture from controller %s with pawn %s."), *PlayerController->GetName(), *Pawn->GetName());
				if (CaptureFromPawn(Pawn))
				{
					return true;
				}
			}
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("CaptureFromLZ failed: no player controller with a capturable pawn was found."));
	return false;
}

UCameraComponent* FFromLZCaptureUtils::FindFromLZCamera(const APawn* Pawn)
{
	if (!Pawn)
	{
		return nullptr;
	}

	TArray<UCameraComponent*> CameraComponents;
	Pawn->GetComponents(CameraComponents);

	for (UCameraComponent* CameraComponent : CameraComponents)
	{
		if (CameraComponent && CameraComponent->GetName() == TEXT("FromLZ"))
		{
			return CameraComponent;
		}
	}

	return nullptr;
}

USpringArmComponent* FFromLZCaptureUtils::FindCameraBoom(const APawn* Pawn)
{
	if (!Pawn)
	{
		return nullptr;
	}

	TArray<USpringArmComponent*> SpringArmComponents;
	Pawn->GetComponents(SpringArmComponents);

	for (USpringArmComponent* SpringArmComponent : SpringArmComponents)
	{
		if (SpringArmComponent && SpringArmComponent->GetName() == TEXT("CameraBoom"))
		{
			return SpringArmComponent;
		}
	}

	return nullptr;
}

TSharedRef<FJsonObject> FFromLZCaptureUtils::SerializeObjectProperties(const UObject* Object)
{
	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();

	if (!Object)
	{
		RootObject->SetStringField(TEXT("error"), TEXT("Null object"));
		return RootObject;
	}

	RootObject->SetStringField(TEXT("object_name"), Object->GetName());
	RootObject->SetStringField(TEXT("class_name"), Object->GetClass()->GetName());

	TSharedRef<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();

	for (TFieldIterator<FProperty> PropertyIt(Object->GetClass(), EFieldIterationFlags::IncludeSuper); PropertyIt; ++PropertyIt)
	{
		const FProperty* Property = *PropertyIt;
		FString ValueText;
		Property->ExportText_InContainer(0, ValueText, Object, Object, const_cast<UObject*>(Object), PPF_None);

		TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
		PropertyObject->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		PropertyObject->SetStringField(TEXT("value_text"), ValueText);

		PropertiesObject->SetObjectField(Property->GetName(), PropertyObject);
	}

	RootObject->SetObjectField(TEXT("properties"), PropertiesObject);
	return RootObject;
}

TSharedRef<FJsonObject> FFromLZCaptureUtils::SerializeTransform(const FTransform& Transform)
{
	TSharedRef<FJsonObject> TransformObject = MakeShared<FJsonObject>();

	const FVector Location = Transform.GetLocation();
	const FRotator Rotation = Transform.Rotator();
	const FVector Scale = Transform.GetScale3D();

	TransformObject->SetNumberField(TEXT("location_x"), Location.X);
	TransformObject->SetNumberField(TEXT("location_y"), Location.Y);
	TransformObject->SetNumberField(TEXT("location_z"), Location.Z);
	TransformObject->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	TransformObject->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	TransformObject->SetNumberField(TEXT("roll"), Rotation.Roll);
	TransformObject->SetNumberField(TEXT("scale_x"), Scale.X);
	TransformObject->SetNumberField(TEXT("scale_y"), Scale.Y);
	TransformObject->SetNumberField(TEXT("scale_z"), Scale.Z);

	return TransformObject;
}

bool FFromLZCaptureUtils::SaveJsonToFile(const TSharedRef<FJsonObject>& JsonObject, const FString& FilePath)
{
	FString OutputString;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutputString);

	if (!FJsonSerializer::Serialize(JsonObject, Writer))
	{
		return false;
	}

	return FFileHelper::SaveStringToFile(OutputString, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
