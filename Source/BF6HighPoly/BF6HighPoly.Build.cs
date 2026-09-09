using System.IO;
using UnrealBuildTool;

public class BF6HighPoly : ModuleRules
{
	public BF6HighPoly(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Projects",
			// The tool itself. Everything reachable from here is the add-on seam
			// in Public/BF6SDKExtension.h; the rest of the tool stays private.
			"BF6UnrealSDK",
			// UStaticMesh built from a MeshDescription: the only kind of mesh
			// Unreal will instance, and the only kind Nanite can be enabled on.
			"MeshDescription",
			"StaticMeshDescription"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			// theme.json: the season palette is data, read at runtime and shared
			// with the Godot plugin. The single-file compile does not link, so a
			// missing module here shows up only in a real build.
			"Json",
			// GMaxRHIShaderPlatform, for the material self-check. The
			// single-file compile does not link, so a missing module here
			// shows up only in a real build.
			"RHI",
			"RenderCore",
			// The ocean compute kernels. They live in their own module because
			// shader types must register at PostConfigInit, before the global
			// shader map is built; declaring one from this module fails DLL
			// static init outright.
			"BF6HighPolyShaders"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// The water path feeds BF6's installed DXIL containers directly to
			// Unreal's public D3D12 interop surface. No exported shader files and
			// no CPU shader translation are involved.
			PrivateDependencyModuleNames.Add("D3D12RHI");
			PublicSystemLibraries.Add("d3d12.lib");
		}

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd", "DesktopPlatform",
				// Builds the preview material properly. Assembling one by hand
				// leaves it half-registered and it never compiles its shaders.
				"MaterialEditor"
			});
		}

		// libbf6's C header. The dll itself is loaded at runtime from the tool's
		// plugin folder, so nothing is linked and no toolchain has to agree.
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "..", "..",
			"..", "BF6UnrealSDK", "Source", "ThirdParty", "libbf6", "include"));
	}
}
