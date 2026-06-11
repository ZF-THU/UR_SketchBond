#pragma once

#include "CoreMinimal.h"

class UWorld;

class FFromLZSessionReset
{
public:
	static void Initialize();
	static void Shutdown();
	static void Tick(UWorld* World);
	static bool HandleGlobalTab(UWorld* World);
	static bool IsResetPending();

	static int32 GetSessionGeneration();
	static bool IsSessionGenerationCurrent(int32 SessionGeneration);
	static void NotifyCompositeTaskStarted();
	static void NotifyCompositeTaskFinished();

private:
	static void FinalizePendingReset(UWorld* World);
};
