# BF6 Unreal SDK: High Poly

The high-detail add-on for [BF6 Unreal SDK](https://github.com/TabbedScamper/BF6_Unreal_SDK).

This is a community-made project. It is not affiliated with EA or DICE.

BF6 Unreal SDK builds Portal maps out of the official SDK's low-poly proxy assets. This add-on reads your own Battlefield 6 install and draws the real thing around them: the actual meshes, the actual materials and textures, the map's terrain, its water and its lighting, as a preview you build inside.

Install it from the SDK's map selector, or copy the release folder into `Plugins/Add-Ons/`. The tool adds a **HIGH POLY** panel and a **LOADOUT** pill for spawner previews. Both components use the existing update button and NEW FEATURES view.

**0.8.2 is available.** Matching package for the SDK experience-block loading and update-recovery hotfix. Save/export unsaved Blocks edits before updating from 0.8.1 or earlier. Streamed textures, Performance and Balanced quality presets, less repeated geometry and material work, fixes for map-unload crashes, corrected mirrored props and water, clickable weapon attachment points, and installed-game artwork for weapon and gadget cards. See [what's new](Resources/CHANGELOG.md) and [performance controls](docs/PERFORMANCE.md). Update the SDK and add-on together.

## What it does

- **The real level, around your map.** Every prop, building and fixture the game places, at its real transform, decoded out of your installed game files rather than any redistributed asset pack.
- **Preview geometry stays local.** The map exports SDK object identities and transforms. Preview loadout choices are stored locally; use the generated script helpers to make weapon attachments affect gameplay.
- **Editable spawner previews.** Select a soldier or loot spawner and press **Space**. Choose characters, outfits, faction, poses, weapons and available attachments. Generate a weapon card or TypeScript helper from the same configuration. Vehicle previews include supported wheels, tracks, cockpit and mounted-weapon parts.
- **Backdrop smoke.** Textured, transparent smoke cards replace untextured white planes. The known packed rising-smoke sheet animates in the viewport.
- **Three modes.** **LOW-POLY** hides everything the add-on built, leaving just your map the way it exports. **CLAY** is the real level in study grey, for shapes and sightlines without the noise of textures. **TEXTURED** is the full thing.
- **Five layers you can switch off.** Terrain, Roads, Objects, Water and Lighting, each with its own cost, so you can build against the ground alone on a big map and turn the rest on when you want to look.
- **Water with the game's own numbers.** Unreal's Single Layer Water shading, fed the level's authored water colour as a per-metre transmission, so absorption and scattering are derived rather than dialled in. Waves come from the map's own ocean simulation entity (direction, speed, chop, wavelength), foam from the wave fold, and the shore fade is read off the terrain heightfield.
- **Lighting the map actually authors.** The sun's real bearing, elevation, colour and illuminance from the level's lighting preset, its sky panorama where the level ships one (a procedural atmosphere where it does not), its fog, and every lamp, spot and lit panel the level places.
- **Terrain and roads.** The heightfield at the shape the game ships, with its ground materials blended per pixel, and the road markings, crossings and wear draped on top of it.
- **Nanite.** Built geometry can be real Nanite static meshes. Slower to build, far better frame rate on a level with tens of thousands of placements.
- **Wind.** A world-position-offset branch on the vegetation material. At rest it costs nothing.
- **Game modes.** A level ships alternative layouts of the same ground. Build one, or stack them and see where they differ.
- **Console variables** for the parts that are still judgement calls: `BF6.HighPoly.SunIntensity`, `BF6.HighPoly.SkyBrightness`, `BF6.HighPoly.WaterExtinction`, `BF6.HighPoly.WaterFoamCoverage`, `BF6.HighPoly.GroundBlend` and friends.

## Requirements

- [BF6 Unreal SDK 0.8.2](https://github.com/TabbedScamper/BF6_Unreal_SDK/releases/tag/v0.8.2), matching this add-on's release
- Unreal Engine 5.8, Windows
- **A legitimate installed copy of Battlefield 6.** The add-on reads assets out of your own install and ships none of its own. On first use it asks where the game is; the folder must contain `bf6.exe`, because the executable carries the type schema that makes the level data readable.

No game assets are redistributed here, and none ever will be. This repository is source code that reads files you already own.

## Install

Recommended: update the SDK to **0.8.0**, then choose **Install High Poly** on its map selector. The tool downloads the matching add-on and prepares the restart needed to install it. Steam and EA App installs can be detected; use the game-folder selector if your install is elsewhere.

Manual install: close Unreal, download **BF6HighPoly_Plugin_v0.8.0.zip** from [Releases](../../releases/latest), and extract its `BF6HighPoly` folder into your project's `Plugins/Add-Ons/`. Reopen the project, open a map, choose HIGH POLY and build. Game assets are read locally and are not included in the download.

To build from source:

1. Clone [BF6 Unreal SDK](https://github.com/TabbedScamper/BF6_Unreal_SDK) and get it opening normally first.
2. Clone this repository into `Plugins/Add-Ons/BF6HighPoly` inside that project.
3. Open the `.uproject`. Unreal discovers the add-on on its own and compiles it; the base project's `.uproject` is never edited.
4. Open a map, press **Space**, pick **HIGH POLY**, point it at your game folder, and build.

## How it is put together

The add-on never touches the tool's internals. It talks to one exported header, `Public/BF6SDKExtension.h` in BF6 Unreal SDK, which registers its radial page, marks its actors as an add-on's, and hands it the current map and the game folder. The API only grows; a version number says what a given tool build offers.

Reading the game is [libbf6](https://github.com/TabbedScamper/BF6_High_Poly_Godot_Plugin), the engine-neutral C++ decode core shared with the Godot version of this plugin, loaded as `bf6_core.dll` from the tool's own folder. What that core knows about Frostbite is written down first in [BF6 Frostbite Research](https://github.com/TabbedScamper/BF6_Frostbite_Research), the format and systems spec this project implements. A fact is learned once and written once; the two engine plugins are thin bindings over it.

The add-on ships no `.uasset` content on purpose. Its materials are built as graphs at runtime, which is what keeps it a folder you can delete.

## Status

This release includes geometry, materials, terrain, roads, water, lighting and editable loadout previews. Cold builds can exceed ten seconds; frame rate depends on the map and hardware. Some specialized materials and water remain approximations. Vehicle coverage varies: unsupported mounts are omitted and reported as an incomplete preview, including the Abrams driver-camera mount. See [the release notes](Resources/CHANGELOG.md) for details.

## Credits

Built and maintained by TabbedScamper.

Frostbite format research with [dfanz0r](https://github.com/dfanz0r), whose static shader recovery is what makes the terrain and water look like the game instead of like a guess.

Battlefield 6 and Battlefield Portal are trademarks of EA Digital Illusions CE AB. This project is not endorsed by or affiliated with EA or DICE.
