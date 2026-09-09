using UnrealBuildTool;

// The ocean compute kernels live here and NOT in BF6HighPoly, for one hard
// reason: IMPLEMENT_GLOBAL_SHADER registers a shader type during the DLL's
// static initialisation, which must happen before the engine builds its global
// shader map. BF6HighPoly loads at Default, long after that, so declaring a
// global shader there fails DLL init outright - LoadLibrary 1114 - and the
// editor dies before it opens.
//
// This module therefore loads at PostConfigInit and does nothing that needs the
// editor to exist: it registers the virtual shader path and the shader types,
// and exposes a plain dispatch API. Keep it that way.
public class BF6HighPolyShaders : ModuleRules
{
	public BF6HighPolyShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",      // IPluginManager, to find our own Shaders directory
			"RHI",
			"RenderCore"
		});
	}
}
