# BF6 Unreal SDK: High Poly

The high-detail add-on for [BF6 Unreal SDK](https://github.com/TabbedScamper/BF6_Unreal_SDK).

This is a community-made project. It is not affiliated with EA or DICE.

BF6 Unreal SDK builds Portal maps out of the official SDK's low-poly proxy assets. This add-on reads your own Battlefield 6 install and draws the real thing around them: the actual meshes, the actual materials and textures, the map's terrain, its water and its lighting, as a preview you build inside.

It is a drop-in folder. Copy it into `Plugins/Add-Ons/`, and the tool grows a **HIGH POLY** page on its build radial. Delete the folder and the tool is exactly what it was.

## What it does

- **The real level, around your map.** Every prop, building and fixture the game places, at its real transform, decoded out of your installed game files rather than any redistributed asset pack.
- **Preview only, by construction.** Nothing the add-on builds is part of your map. Its actors carry none of the tool's own tags, so the exporter, the budget bar, the scene tree, the placement rays and the save file cannot see them. There is no setting to get this wrong.
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

- [BF6 Unreal SDK](https://github.com/TabbedScamper/BF6_Unreal_SDK), with the add-on extension API (v2 or newer)
- Unreal Engine 5.8, Windows
- **A legitimate installed copy of Battlefield 6.** The add-on reads assets out of your own install and ships none of its own. On first use it asks where the game is; the folder must contain `bf6.exe`, because the executable carries the type schema that makes the level data readable.

No game assets are redistributed here, and none ever will be. This repository is source code that reads files you already own.

## Install

There are no releases yet. Build it from source:

1. Clone [BF6 Unreal SDK](https://github.com/TabbedScamper/BF6_Unreal_SDK) and get it opening normally first.
2. Clone this repository into `Plugins/Add-Ons/BF6HighPoly` inside that project.
3. Open the `.uproject`. Unreal discovers the add-on on its own and compiles it; the base project's `.uproject` is never edited.
4. Open a map, press **Space**, pick **HIGH POLY**, point it at your game folder, and build.

## How it is put together

The add-on never touches the tool's internals. It talks to one exported header, `Public/BF6SDKExtension.h` in BF6 Unreal SDK, which registers its radial page, marks its actors as an add-on's, and hands it the current map and the game folder. The API only grows; a version number says what a given tool build offers.

Reading the game is [libbf6](https://github.com/TabbedScamper/BF6_High_Poly_Godot_Plugin), the engine-neutral C++ decode core shared with the Godot version of this plugin, loaded as `bf6_core.dll` from the tool's own folder. What that core knows about Frostbite is written down first in [BF6 Frostbite Research](https://github.com/TabbedScamper/BF6_Frostbite_Research), the format and systems spec this project implements. A fact is learned once and written once; the two engine plugins are thin bindings over it.

The add-on ships no `.uasset` content on purpose. Its materials are built as graphs at runtime, which is what keeps it a folder you can delete.

## Status

Early, and honest about it. Geometry, materials, terrain, roads, water, lighting, Nanite, wind and the mode and layer switches all work on real maps. Known rough edges: terrain material composition is still being finished against the recovered layer model, some vegetation and glass materials are approximations rather than the recovered shader, and the ocean's frame-exact displacement waits on the game's own FFT compute kernels (the current waves are a Gerstner approximation derived from the authored simulation inputs).

## Credits

Built and maintained by TabbedScamper.

Frostbite format research with [dfanz0r](https://github.com/dfanz0r), whose static shader recovery is what makes the terrain and water look like the game instead of like a guess.

Battlefield 6 and Battlefield Portal are trademarks of EA Digital Illusions CE AB. This project is not endorsed by or affiliated with EA or DICE.
