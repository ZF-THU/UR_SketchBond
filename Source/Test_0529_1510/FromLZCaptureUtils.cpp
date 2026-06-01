#include "FromLZCaptureUtils.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/Scene.h"
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
#include "Math/Float16Color.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"

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
	SCC->FOVAngle = Camera->FieldOfView;
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

	// GPU pass 1: linear scene depth (world units in the red channel).
	SCC->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	SCC->TextureTarget = DepthRT;
	SCC->CaptureScene();

	// GPU pass 2: world-space normal.
	SCC->CaptureSource = ESceneCaptureSource::SCS_Normal;
	SCC->TextureTarget = NormalRT;
	SCC->CaptureScene();

	FlushRenderingCommands();

	TArray<FFloat16Color> DepthPixels;
	TArray<FFloat16Color> NormalPixels;
	FTextureRenderTargetResource* DepthRes = DepthRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* NormalRes = NormalRT->GameThread_GetRenderTargetResource();

	const bool bReadOk =
		DepthRes && DepthRes->ReadFloat16Pixels(DepthPixels) &&
		NormalRes && NormalRes->ReadFloat16Pixels(NormalPixels) &&
		DepthPixels.Num() == Size.X * Size.Y &&
		NormalPixels.Num() == Size.X * Size.Y;

	bool bSaved = false;

	if (bReadOk)
	{
		const int32 W = Size.X;
		const int32 H = Size.Y;

		TArray<float> Depth;
		TArray<FVector3f> Normal;
		Depth.SetNumUninitialized(W * H);
		Normal.SetNumUninitialized(W * H);
		for (int32 i = 0; i < W * H; ++i)
		{
			Depth[i] = DepthPixels[i].R.GetFloat();
			Normal[i] = FVector3f(NormalPixels[i].R.GetFloat(), NormalPixels[i].G.GetFloat(), NormalPixels[i].B.GetFloat());
		}

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

				const float Dgx =
					-SampleDepth(x - 1, y - 1) - 2.f * SampleDepth(x - 1, y) - SampleDepth(x - 1, y + 1) +
					 SampleDepth(x + 1, y - 1) + 2.f * SampleDepth(x + 1, y) + SampleDepth(x + 1, y + 1);
				const float Dgy =
					-SampleDepth(x - 1, y - 1) - 2.f * SampleDepth(x, y - 1) - SampleDepth(x + 1, y - 1) +
					 SampleDepth(x - 1, y + 1) + 2.f * SampleDepth(x, y + 1) + SampleDepth(x + 1, y + 1);
				const float DepthGrad = FMath::Sqrt(Dgx * Dgx + Dgy * Dgy);
				const bool bDepthEdge = (DepthGrad / FMath::Max(Dc, 1.f)) > DepthRelThreshold;

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

	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("capture_timestamp"), Timestamp);
	RootObject->SetStringField(TEXT("json_path"), JsonPath);
	RootObject->SetStringField(TEXT("png_path"), PngPath);
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
	ViewObject->SetStringField(TEXT("projection_mode"), StaticEnum<ECameraProjectionMode::Type>()->GetValueAsString(CameraComponent->ProjectionMode));
	ViewObject->SetNumberField(TEXT("ortho_width"), CameraComponent->OrthoWidth);
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
