#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

// A COLLAPSIBLE SECTION, drawn and animated the way highpoly_section.gd is.
//
//   closed   the title is centred and white, over a white line that fades out
//            at both ends and stops short of the panel edges.
//   hover    title and line warm to the accent over HL_SECS.
//   opening  the description goes, the title settles solid accent, and the
//            contents RISE OUT FROM BEHIND THE LINE as they fade in, starting
//            RISE pixels above their resting place.
//   closing  the same in reverse.
//
// The line is DRAWN, not themed, for the same reason it is in Godot: a border
// or a separator widget cannot fade its own ends, and a hard rule touching both
// panel edges is the thing this design is avoiding.
//
// The contents live in a CLIPPED box whose height is what actually animates, so
// the sections below reflow as this one grows instead of being overlapped, and
// the contents slide from above zero inside that box - which is what reads as
// coming out from behind the line rather than merely appearing.
//
// Geometry and timings are taken from highpoly_section.gd verbatim.
class SBF6HighPolySection : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6HighPolySection)
		: _Title()
		, _Description()
		, _InitiallyOpen(false)
	{}
		SLATE_ARGUMENT(FString, Title)
		SLATE_ARGUMENT(FString, Description)
		SLATE_ARGUMENT(bool, InitiallyOpen)
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	bool IsOpen() const { return bOpen; }
	void SetOpen(bool bInOpen, bool bAnimate = true);

private:
	FString Title;
	FString Description;
	bool    bOpen = false;
	bool    bHovered = false;

	TSharedPtr<class SBox>   Body;      // the clipped box whose height animates
	TSharedPtr<class SWidget> Contents; // what rises out of it

	// ONE TWEEN. From, To, how long, and how far through it is. Restarting from
	// the CURRENT displayed value is what makes an interrupted open close
	// smoothly from where it got to, rather than snapping first - which is what
	// Godot's tween.kill() then a fresh tween does.
	struct FTween
	{
		float From = 0.f, To = 0.f, Elapsed = 0.f, Duration = 0.f;
		float Value() const;
		bool  IsDone() const { return Elapsed >= Duration; }
		void  To_(float NewTo, float Secs);
		void  Advance(float Delta) { Elapsed = FMath::Min(Elapsed + Delta, Duration); }
		void  Snap(float V) { From = To = V; Elapsed = Duration = 0.f; }
	};
	FTween Reveal;   // 0 shut, 1 open: drives the height and the rise
	FTween Fade;     // the contents' opacity
	FTween Warm;     // 0 white, 1 accent

	TWeakPtr<FActiveTimerHandle> Anim;
	EActiveTimerReturnType OnAnimate(double InTime, float InDelta);
	void Kick();     // make sure the animation timer is running

	float Highlight() const { return Warm.Value(); }
	FReply OnHeaderClicked();
	void   OnHoverChanged(bool bIn);

	/** The height the contents want, for the box to animate towards. */
	float ContentHeight() const;
};

// The header on its own: the centred title and the fading line. Split out
// because the line has to be painted, and a painting widget that also has a
// child is harder to reason about than two widgets.
class SBF6HighPolySectionHead : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6HighPolySectionHead) {}
		SLATE_ARGUMENT(FString, Title)
		SLATE_ATTRIBUTE(float, Highlight)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float) const override;

private:
	FString Title;
	TAttribute<float> Highlight;
};
