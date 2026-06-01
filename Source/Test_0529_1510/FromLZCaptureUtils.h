#pragma once

#include "CoreMinimal.h"

class APawn;
class FJsonObject;
class UCameraComponent;
class UObject;
class USpringArmComponent;
class UWorld;

class FFromLZCaptureUtils
{
public:
	static bool CaptureFromPawn(const APawn* Pawn);
	static bool CaptureFromWorld(const UWorld* World);
	static UCameraComponent* FindFromLZCamera(const APawn* Pawn);
	static USpringArmComponent* FindCameraBoom(const APawn* Pawn);
	static TSharedRef<FJsonObject> SerializeObjectProperties(const UObject* Object);
	static TSharedRef<FJsonObject> SerializeTransform(const FTransform& Transform);
	static bool SaveJsonToFile(const TSharedRef<FJsonObject>& JsonObject, const FString& FilePath);
};
