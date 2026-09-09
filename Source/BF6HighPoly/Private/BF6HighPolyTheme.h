#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/CoreStyle.h"

// ============================================================================
// THE ADD-ON'S LOOK, TAKEN FROM THE GODOT PLUGIN RATHER THAN INVENTED.
//
// The High Poly preview plugin in the SDK already has a look, and it is the one
// the user built: a season palette in theme.json, a display face, and a set of
// rules about how a panel is put together. The first version of the Unreal
// panel ignored all of it and used the editor's default grey with an orange
// accent, which is not even the fallback the Godot side uses when its palette
// is missing. It matched nothing.
//
// THE RULES, from highpoly_theme.gd, and worth stating because they are not
// obvious from the colours alone:
//
//   accent      the panel outline, section headings and progress fills
//   heading     heading text
//   splash_bg   the opaque floor the panel sits on
//   controls    white on translucent white, at four interaction strengths.
//               They take NO colour from the palette. A control tinted with
//               the accent is wrong even though it would look co-ordinated.
//   outline     drawn, with the centre NOT filled on the Godot side because a
//               video plays behind it. There is no video here, so the centre is
//               the splash colour and the outline stays.
//
// THE PALETTE IS DATA. Its own note says a colour change should need no plugin
// release, so it is read at runtime, from the SDK's copy when there is one so a
// season change there shows up here too, and from the copy shipped beside this
// add-on otherwise.
// ============================================================================
namespace BF6HPTheme
{
	// Season palette. Falls back to the same values the Godot plugin falls back
	// to, so a missing file still looks deliberate rather than invented.
	FLinearColor Accent();
	FLinearColor AccentDim();
	FLinearColor Heading();
	FLinearColor SplashBg();
	FString      SeasonName();

	// Controls are white on translucent white. The strengths are _mask(fill,
	// border) on the Godot side, and BOTH numbers matter: a mask with the right
	// fill and no border reads as a smudge rather than a control.
	//
	//   Rest      0.10 / 0.55        Chip off     0.08 / 0.45
	//   Hover     0.20 / 0.85        Chip hover   0.18 / 0.80
	//   Press     0.30 / 1.00        Chip ON      accent fill, white border
	//   Disabled  0.04 / 0.16        Chip on-hover accent lightened
	//
	// A chip that is switched ON is the one control that takes the accent, and
	// it is why the accent is not spent anywhere else in a row.
	enum class EMask : uint8 { Rest, Hover, Press, Disabled, ChipOff, ChipHover, ChipOn, ChipOnHover };
	const FSlateBrush* Mask(EMask M);

	// A control's border is drawn separately from its fill, because Slate has no
	// single brush that does both the way a Godot StyleBoxFlat does.
	FLinearColor MaskBorder(EMask M);

	// A flat brush of one colour, cached. Slate keeps a raw pointer to a brush
	// for the life of the widget, so these must outlive the panel: they are
	// owned here and never handed out by value.
	const FSlateBrush* Solid(const FLinearColor& C);

	// The season display face, at the panel's own size. Null when the face is
	// not installed, in which case the caller uses the editor's font.
	FSlateFontInfo Font(int32 Size);

	// Re-read theme.json. The palette is data and can change under a running
	// editor; nothing caches a colour beyond this call.
	void Reload();
}
