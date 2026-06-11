#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

class FFromLZFaceReconstructor
{
public:
	static void ProcessPress(const FString& PressDir, const FString& ActionPressDir, TWeakObjectPtr<UWorld> World, int32 SessionGeneration = INDEX_NONE);
	static void RestoreStep11RuntimeBooleans(TWeakObjectPtr<UWorld> World);
	static void ResetAllRuntimeState(TWeakObjectPtr<UWorld> World);
	static bool IsStep11RuntimeActor(const AActor* Actor);
	static bool IsStep11RuntimeActorActiveForCapture(const AActor* Actor);
};
