#pragma once

#include "BF6HighPolyControls.h"   // FControl: what the panel and the radial both render

#include "CoreMinimal.h"

// ============================================================================
// PLACED OBJECTS IN HIGH POLY.
//
// The tool draws what the creator places as a white procedural mesh read from
// the SDK's own low-poly model. This module puts the real thing on top: the
// pf_portal_<name> prefab the game assembles for that placeable, decoded from
// the install through libbf6, built with the add-on's materials, and hung on
// the placed actor so it moves, duplicates, deletes and selects with it. The
// prefab's own light fixtures come along as real light components.
//
// Three user-facing switches, all reachable from the HIGH POLY ring and the
// console:
//   BF6.HighPoly.Lights 0|1            every light the add-on owns, at once
//   BF6.HighPoly.PreviewSelected 0|1   selection shows the OTHER detail level
//   BF6.HighPoly.Placed.Status         what the module currently holds
//
// Self-registering: a static initialiser defers to OnPostEngineInit, so
// BF6HighPoly.cpp's module class does not know this file exists.
// ============================================================================
namespace BF6HP
{
namespace Placed
{
	void Start();
	void Stop();

	// What this module contributes to the High Poly panel and radial.
	void AddControls(TArray<FControl>& R);

	// The detail-distance slider and its override, kept separate so the panel
	// can draw them above the BUILD button rather than inside a section.
	void AddDistanceControls(TArray<FControl>& R);

	// The master switch for placed-object high poly. Turning it back ON also
	// lifts a stop left by a resolve that ran away, because otherwise the
	// switch reads as on and still nothing happens.
	void SetEnabled(bool bOn);
	bool Enabled();

	// The global light switch (map lights and placed-object lights together).
	void SetLightsOn(bool bOn);
	bool LightsOn();

	// The per-selection override: selected placed objects show the opposite
	// representation of the current mode.
	void SetPreviewSelected(bool bOn);
	bool PreviewSelected();

	// Forget every built type and re-resolve. Cheap to ask for, paced to run.
	void RebuildAll();

	// Allow the objects that were skipped for taking too long to be tried once
	// more. Nothing else clears that list: it is what keeps one runaway prefab
	// from freezing the editor a second time.
	void RetrySkipped();

	// What the resolver is inside RIGHT NOW, or empty. The frame watch reports
	// it, because a resolve is game-thread work of ours and reporting a five
	// second frame as "nothing of ours" was worse than saying nothing.
	FString Resolving();
	// Advances the same placed-object queue while the Build dialog owns the UI.
	// Counts geometry and enabled fixture lighting, including explicit fallbacks.
	bool FinishBuildStep(int32& Done, int32& Total, FString& Error);

	// One line for the ring's sub text: "12 of 40 high-poly" and the like.
	// HOW FAR AWAY THE REAL MODELS ARE WORTH DRAWING.
	//
	// Every placed object is one draw call carrying its full detail, so the
	// distant three thousand cost as much as the near dozen. Past this distance
	// the add-on stops drawing its model and lets the SDK blockout draw again -
	// a swap, not a hole, so the scene is complete at any range. 10 to 1000
	// metres, 100 by default.
	void  SetCullMetres(float Metres);
	float CullMetres();

	// Ignore that distance and draw everything in full, however far away.
	// Truest to the game and the most expensive thing the add-on can be asked
	// to do. Off by default.
	void  SetNoCull(bool bOn);
	bool  NoCull();

	FString StatusLine();
}
}
