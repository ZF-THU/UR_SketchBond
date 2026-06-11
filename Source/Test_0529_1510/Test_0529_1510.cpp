// Copyright Epic Games, Inc. All Rights Reserved.

#include "Test_0529_1510.h"

#include "FromLZSessionReset.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

// Custom game module: on startup, reset the project's working folders under Saved/ so every
// program run begins from a clean state. Existing folders are emptied (best-effort; files
// that are still in use, e.g. the active engine log, are skipped) and recreated.
class FTest_0529_1510Module : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		const FString SavedDir = FPaths::ProjectSavedDir();

		// Cleared (emptied + recreated) on every run.
		const TCHAR* ClearDirs[] =
		{
			TEXT("2DDebug"),
			TEXT("FromAction"),
			TEXT("FromLZCaptures"),
			TEXT("FromProcess"),
			TEXT("Logs"),
		};
		// Only ensured to exist; contents are preserved (e.g. input sketches).
		const TCHAR* EnsureDirs[] =
		{
			TEXT("FromSketch"),
		};

		IFileManager& FM = IFileManager::Get();
		for (const TCHAR* Sub : ClearDirs)
		{
			const FString Dir = SavedDir / Sub;
			if (FM.DirectoryExists(*Dir))
			{
				FM.DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
			}
			FM.MakeDirectory(*Dir, /*Tree*/ true);
		}
		for (const TCHAR* Sub : EnsureDirs)
		{
			FM.MakeDirectory(*(SavedDir / Sub), /*Tree*/ true);
		}

		FFromLZSessionReset::Initialize();
	}

	virtual void ShutdownModule() override
	{
		FFromLZSessionReset::Shutdown();
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE( FTest_0529_1510Module, Test_0529_1510, "Test_0529_1510" );
