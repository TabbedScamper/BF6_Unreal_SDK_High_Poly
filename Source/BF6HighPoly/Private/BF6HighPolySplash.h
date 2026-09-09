#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

// ============================================================================
// THE PANEL'S ENTRANCE, THE SAME ONE THE GODOT PLUGIN PLAYS.
//
// From highpoly_splash.gd, and deliberately the same numbers rather than
// something that felt about right:
//
//   1  waves fill the panel, alone                          1.30s
//   2  the Portal logo fades in, holds, fades out      0.30 / 1.50 / 0.70
//   3  the backdrop settles to a dark tint                  1.20s
//      and the controls fade in, starting part way     0.30 then 0.85
//   4  the waves KEEP LOOPING under everything, for good
//
// A click anywhere skips to the end, for the times you just want the buttons.
//
// WHY A SPRITE SHEET AND NOT THE VIDEO. The Godot side plays waves.ogv, which
// is Ogg Theora, and Unreal has no decoder for it: its media plugins cover WMF,
// WebM and Electra formats, and none of them read Theora. There is no ffmpeg on
// this machine either, and the original mp4 the video was made from is gone.
//
// So Godot, which owns the format, was used once, offline, to decode the loop
// into a sheet of frames, and that sheet ships. It is half the frame rate and a
// third of the size of the original, which is invisible behind the 0.72 dim in
// front of it and takes the backdrop from 29 MB of frames to about one. The
// extractor is kept beside the art in Resources/theme/tools so a new season's
// video can be turned into a new sheet the same way.
// ============================================================================
class SBF6HighPolySplash : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6HighPolySplash) {}
		// The panel itself. It fades in during stage 3 and is what remains.
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Slate calls these; they are public only because the framework needs them.
	virtual void Tick(const FGeometry& Geo, const double Now, const float Delta) override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geo, const FSlateRect& Culled,
		FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle& Style, bool bParentEnabled) const override;
	virtual FReply OnMouseButtonDown(const FGeometry& Geo, const FPointerEvent& Ev) override;
	virtual bool SupportsKeyboardFocus() const override { return false; }

	// True when the artwork is installed. With no sheet and no logo there is
	// nothing to play and the panel is shown directly, exactly as the Godot
	// plugin does on an install with no artwork.
	static bool ArtworkPresent();

private:
	enum class EStage : uint8 { Waves, Logo, Settle, Done };

	void Skip();
	void SkipEmpty();
	float LogoAlpha() const;

	EStage Stage = EStage::Waves;
	float  T = 0.f;
	float  TintA = 0.f;      // the dim in front of the waves
	float  UiA = 0.f;        // the controls
	double LastTick = 0.0;
	float  WaveT = 0.f;      // seconds into the loop, kept across stages
};
