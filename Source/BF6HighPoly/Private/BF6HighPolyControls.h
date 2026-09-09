#pragma once

#include "CoreMinimal.h"

// ============================================================================
// ONE CONTROL, DESCRIBED WELL ENOUGH TO DRAW PROPERLY.
//
// This exists because the panel first reused the radial's own entry type,
// BF6Ext::FPieSubEntry: a label, a status line and something to run when
// picked. That is enough to build a button and nothing else, so every control
// in the panel came out as a button. Picking a mode was three buttons rather
// than a dropdown, every switch was a button that flipped, and GAME MODE opened
// the radial menu from inside the panel that exists so the radial is not
// needed.
//
// The Godot plugin's panel uses OptionButton for a choice, HSlider for a value,
// CheckBox for a switch, and Button only for something that actually happens.
// To render that, a control has to say WHICH of those it is and carry the state
// behind it, which is all this type adds.
//
// The radial is still built from the same list, because sharing the list is
// what stops the two surfaces drifting: an action becomes a pill, a switch a
// pill that flips, a choice a pill that steps to the next option.
// ============================================================================
struct FControl
{
	enum class EKind : uint8 { Action, Toggle, Choice, Slider };

	EKind   Kind = EKind::Action;
	FString Label;
	FString Tip;
	TFunction<FString()> Sub;          // the live status line, every kind

	// Action
	TFunction<void()>      OnAct;
	bool                   bCloses = false;   // closes the radial when picked

	// Toggle
	TFunction<bool()>      Get;
	TFunction<void(bool)>  Set;

	// Choice
	TArray<FString>        Choices;
	TFunction<int32()>     GetChoice;
	TFunction<void(int32)> SetChoice;

	// Slider
	TFunction<float()>     GetValue;
	TFunction<void(float)> SetValue;
	float Min = 0.f, Max = 1.f, Step = 0.05f;
};

struct FControlSection
{
	const TCHAR* Name;
	TArray<FControl> Entries;
};
