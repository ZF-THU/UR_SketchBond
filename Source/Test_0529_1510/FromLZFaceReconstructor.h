#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

struct FFromLZCandidateFaceRequest
{
	FString CandidateSource;
	FString Action;
	TArray<FVector2D> CapPolygon;
	TArray<FVector2D> CapPolygonTranslated;
	TArray<FVector2D> SideVectors;
};

struct FFromLZCandidateFaceEvaluation
{
	bool bEvaluated = false;
	bool bValid = false;
	FString SourcePolygonKey;
	FString RejectReason;
	int32 CapMaskPixels = 0;
	int32 SelectedFaceId = -1;
	int32 SelectedFaceOverlapPixels = 0;
	double SelectedFaceOverlapRatio = 0.0;
	double SelectedFaceNormalSideAngleDegrees = -1.0;
	double SelectedFaceDistanceToCamera = 0.0;
	FVector SelectedPlaneHit = FVector::ZeroVector;
};

class FFromLZFaceReconstructor
{
public:
	static constexpr double CandidateFaceMinOverlapRatio = 0.25;
	static constexpr double CandidateFaceMaxNormalSideAngleDegrees = 30.0;
	static constexpr double CandidateFacePreferredNormalSideAngleDegrees = 10.0;

	static bool EvaluateCandidateFaces(
		const FString& PressDir,
		int32 SourceWidth,
		int32 SourceHeight,
		const TArray<FFromLZCandidateFaceRequest>& Requests,
		TArray<FFromLZCandidateFaceEvaluation>& OutEvaluations,
		FString& OutError);
	static void ProcessPress(const FString& PressDir, const FString& ActionPressDir, TWeakObjectPtr<UWorld> World, int32 SessionGeneration = INDEX_NONE);
	static void RestoreStep11RuntimeBooleans(TWeakObjectPtr<UWorld> World);
	static void ResetAllRuntimeState(TWeakObjectPtr<UWorld> World);
	static bool IsStep11RuntimeActor(const AActor* Actor);
	static bool IsStep11RuntimeActorActiveForCapture(const AActor* Actor);
};
