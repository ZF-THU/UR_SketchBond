// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class Test_0529_1510 : ModuleRules
{
	public Test_0529_1510(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "Json", "JsonUtilities", "ImageWrapper", "RenderCore", "ProceduralMeshComponent", "GeometryCore" });

		// Slate/SlateCore are used by the runtime sketch board and live preview overlays.
		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Manifold is vendored under ThirdParty and compiled through the small wrapper
		// translation units in Source/Test_0529_1510/ManifoldBuild.
		string ManifoldPath = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "ThirdParty", "Manifold", "manifold"));
		PublicIncludePaths.Add(Path.Combine(ManifoldPath, "include"));
		PrivateIncludePaths.Add(Path.Combine(ManifoldPath, "src"));
		PrivateDefinitions.AddRange(new string[]
		{
			"MANIFOLD_PAR=-1",
			"MANIFOLD_CROSS_SECTION=0",
			"MANIFOLD_NO_IOSTREAM=1",
			"MANIFOLD_NO_FILESYSTEM=1",
			"TRACY_ENABLE=0",
			"TRACY_MEMORY_USAGE=0"
		});
	}
}
