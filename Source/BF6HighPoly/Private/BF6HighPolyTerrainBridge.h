#pragma once

#include "CoreMinimal.h"

// Native seam used by the UE 5.8 MCP-facing Blueprint library.  The terrain
// implementation remains private to BF6HighPoly.cpp; these calls only schedule
// the current-install DXIL page and report its live state.
namespace BF6HighPolyTerrainBridge
{
	bool StartAtVisibleCamera(float SpanMetres, int32 Resolution);
	void Disable();
	FString Status();
}
