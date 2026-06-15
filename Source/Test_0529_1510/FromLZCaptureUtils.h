#pragma once

#include "CoreMinimal.h"

class APawn;
class FJsonObject;
class UCameraComponent;
class UObject;
class USpringArmComponent;
class UWorld;
class FViewport;

class FFromLZCaptureUtils
{
public:
	static bool BeginCaptureFromWorld(const UWorld* World, FViewport* Viewport);
	static bool BeginCaptureFromTaggedCamera(const UWorld* World, FViewport* Viewport, FName CameraActorTag);
	static void CancelPendingCapture();
	static void NotifyViewportDrawn(const UWorld* World, FViewport* Viewport);
	static void CompletePendingCapture(const UWorld* World, FViewport* Viewport);
	static UCameraComponent* FindFromLZCamera(const APawn* Pawn);
	static USpringArmComponent* FindCameraBoom(const APawn* Pawn);
	static TSharedRef<FJsonObject> SerializeObjectProperties(const UObject* Object);
	static TSharedRef<FJsonObject> SerializeTransform(const FTransform& Transform);
	static bool SaveJsonToFile(const TSharedRef<FJsonObject>& JsonObject, const FString& FilePath);
};
