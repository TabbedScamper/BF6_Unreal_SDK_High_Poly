#pragma once

// ============================================================================
// The game's own UI sounds, given to the tool.
//
// The tool raises events. It has no idea what a Battlefield 6 install is and it
// ships no audio. This is the other half: it reads the player's own install
// through libbf6, decodes the menu-navigation sounds the game itself plays, and
// registers as the tool's sound provider. Delete the add-on and the tool is
// silent again with nothing left behind.
//
// It also answers the object library's SFX_* rows. Nine hundred and forty one
// of the placeables a user can drop into a Portal map are SOUNDS, chosen today
// entirely by reading a name. A play button on the row plays the sound the game
// would play for that object, out of the user's install.
//
// WHAT THE MOUNT COSTS, measured on retail:
//
//   mount_all(0)   common/sound/ui/ 1075 partitions - the front end's own
//                  menu-navigation style. Seconds.
//   mount_all(1)   adds common/sound/portal/ (2776) and the mod builder's
//                  audio (101) - the Portal front end's own voice, the
//                  objective stingers, and every SFX_* placeable's sound.
//                  Eighty five seconds cold, and the add-on already pays it
//                  for the object library's pictures.
//
// So this attaches in two passes. The first is cheap and gives every event a
// voice from the shared mount. The second happens for free the moment anything
// else in the add-on mounts the catalogue, and upgrades the events that have a
// better Portal-authored sound waiting for them.
// ============================================================================

#include "CoreMinimal.h"

namespace BF6HP { class FCore; }

namespace BF6HPUiSounds
{
	// Start the worker and register both providers. The core is the add-on's
	// own; it may not be open yet, and this opens it the same way the previews
	// do. Safe to call twice.
	void Attach(BF6HP::FCore* Core);

	// Unregister, stop anything playing, release the waves. Call before the
	// module's own teardown releases bf6_core.dll.
	void Detach();

	// Something else mounted the whole catalogue. Re-resolve on the worker so
	// the Portal-authored sounds replace the shared-mount fallbacks. Cheap and
	// idempotent; ignored when the catalogue is not actually mounted.
	void OnCatalogueMounted();
}
