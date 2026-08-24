#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

// The High Poly add-on.
//
// It attaches to the BF6 Unreal SDK through the add-on seam and nothing else:
// one pill on the build radial, its own popups, its own actors. Deleting the
// plugin folder removes it completely, and the tool behaves as though it was
// never installed.
class FBF6HighPolyModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
