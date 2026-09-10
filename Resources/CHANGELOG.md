# BF6 High Poly version history

## 0.8.1 (2026-09-10)

0.8.1 brings streamed textures, viewport quality choices, more efficient scene construction, fixes for map-unload crashes and water, and a visual attachment picker connected to the SDK's weapon-card designer. Requires BF6 Unreal SDK 0.8.1 and its matching game reader.

**Water and mirrored scenery**

- Fixed inside-out mirrored map placements, including Manhattan Bridge anchorage walls and steps. Mirrored instances use separate batches with reflection on the component, preserving positions and shared meshes across conventional and Nanite rendering.
- Fixed the camera-following water circle on maps with fewer than four wave cascades. Disabled normal layers now contribute zero slope instead of decoding as a tilted surface. Missing absorption data also uses the existing water fallback instead of zero absorption and scattering.

**Smoother building and lower memory pressure**

- Prevented shutdown from unloading the game reader while game-mode mining is still executing. Queued work is counted before dispatch, new work stops during shutdown, and late results do not update the UI.
- Shared identical render corners in conventional meshes while retaining authored UV, normal and palette seams.
- Moved ocean FFT calculations off the editor thread with bounded work, exact replay checks and safe reset handling.
- Stopped new parent-material compilation from requesting a global rebuild of unrelated scene render proxies.
- Included incomplete shader variants after a quality change in build finalization, and added actual parent shader-map completeness to automated readiness checks.
- Added real mip streaming for runtime material textures, including recoverable mip data and UV-density metadata for fast-built props. Streaming workers release cancelled requests correctly during map changes.
- Released duplicate native texture payloads after copying and invalidated capped decodes when levels change. Texture cache identities now distinguish reader sessions.
- Added Performance and Balanced viewport quality choices for lighting, shadows and effects.
- Bounded optional derived-cache writes and mesh preparation batches, and collected destroyed preview resources before rebuilding.
- Bounded terrain construction memory without changing the terrain grid, and batched water tile data to avoid redundant instance-hierarchy builds during camera movement.
- Reused the mounted fixture-name index across light queries, including short prefab names, while preserving the original lighting results.
- Added an experimental authored-LOD comparison for compatible conventional map props, plus CPU/render/GPU timing capture for automated tests. Authored LODs remain off by default pending broader visual and hardware validation.

**Crash fixes and diagnostics**

- Shared material instances no longer retain the actor or mesh that first requested them. This fixes a world-unload crash exposed by the automated test after building High Poly and opening another Unreal level.
- Added on-demand build-completion and texture-residency snapshots for the SDK's automated stability tests. Reports distinguish textures actually registered for streaming from textures that merely allow it.
- Fixed material, texture and colour lookup caches retaining raw object pointers after Unreal garbage collection. Cached resources now remain alive until the map cache is cleared, and are released during add-on shutdown. This addresses a lifetime defect on the material-binding path identified in a released-build crash report.

**Attachment previews, prop colours and equipment cards**

- Added a side preview of the selected loot weapon with clickable attachment points. Available points open searchable choices with names and images from the installed game's attachment atlas; the slot dropdowns remain available below.
- Preserved per-vertex prop palette colours through mesh merging, Unreal materials and disk caching. This restores separately tinted members within supported architecture sections, including the Manhattan bridge's blue and grey palette entries.
- Kept the selected material variation when recovering a mesh from an unavailable placement scope. Updated preview thumbnail caches for the material changes.
- Added visible feedback for loot binding actions. Opening a weapon card closes the loadout panel and reveals the linked UI design.
- Improved selected attachment placement by using the updated SDK reader's composed weapon pose.
- Connected installed weapon and gadget artwork to the UI designer. Weapon cards assemble the configured parts, center the complete image, and scale long barrels and suppressors to fit.
- Updated attachment thumbnails to decode their original atlas dimensions and use separate distance-field outline and fill coverage.
- The selected weapon's loadout preview can now be requested in Low Poly without replacing every scene spawner. Low Poly also releases geometry priority so background artwork requests can finish.

**Installation and current limits**

- Install from the SDK map selector or use the shared Update button for an existing install. NEW FEATURES includes the add-on's own 0.8.1 notes.
- Preview assets continue to come from the creator's own installed game. Extracted scenery and preview images are not included in Portal exports or these downloads.
- Full material parity for secondary UVs and overlays, complete attachment visibility/conflict rules, and an exhaustive check of weapon configurations remain in progress. Card styling and framing still need comparison with the live Portal renderer.
- Performance depends on the level, hardware and enabled layers. The test harness does not certify ten-second cold opens or a locked 60 FPS on every 6 GB graphics card. Authored LODs remain experimental; use the Performance preset and texture streaming for the supported controls in this release.

## 0.8.0 (2026-09-09)

The first High Poly release numbered alongside the base SDK brings the installed game's scenery and configurable previews into the same map-building workflow. It requires BF6 Unreal SDK 0.8.0.

**Explore the real map while you build**

- Read the current level's geometry, materials and textures from your own Battlefield 6 installation, surrounding your editable SDK objects with the game's scenery.
- Choose Low Poly for the SDK view, Clay for shapes and sightlines, or Textured for the detailed preview.
- Switch Terrain, Roads, Objects, Water and Lighting independently to concentrate on the layers you need.
- Preview geometry stays local. Portal exports retain SDK identities and transforms rather than including extracted high-detail meshes.
- Switching maps clears the previous scenery. Results from an old map's background build are discarded rather than appearing in the next map.

**One High Poly panel**

- HIGH POLY opens an organized panel with the current map and installation status at the top.
- Controls are grouped into look, layers, game mode, build, placed objects and previews, water, and setup.
- Live rows show progress, preview counts and failures without making you reopen the panel.
- Water Lab is accessible from the panel. The alternate ring remains available through BF6.HighPoly.Ring.

**Soldiers and customizable loot**

- Select a supported soldier or loot spawner and press Space, or open the Loadout pill.
- Soldier spawners display a posed character holding a weapon. Choose supported characters, outfits, factions and poses.
- Loot spawners default to an M4A1 positioned against the SDK marker. Choose a weapon, gadget or throwable and configure supported weapon attachments.
- Selections are saved as local preview metadata. Preview components belong to the editable spawner so selection can reach the underlying object.
- Generate a weapon card from the same loadout and edit it in the SDK's UI builder. Updating the weapon configuration preserves an existing card layout.
- Send the configuration to Blocks as a base-loot recipe, or to Script as a configured pickup helper. Use those building pieces in your own proximity prompts, buy stations and weapon progression.
- Gameplay attachments require mode logic: the helper replaces a confirmed pickup in its weapon slot with the configured attachment package. Preview choices alone do not change Portal gameplay.

**Vehicles and placed objects**

- Vehicle previews assemble supported third-person wheels, tracks, cockpit and mounted-weapon parts, including repeated instances.
- Supported mounts use the vehicle or attachment skeleton rather than placing every member at the root.
- Placed-object resolution follows asset metadata and the current level's material scopes, improving replacements for renamed and imported SDK objects.
- Nearby placed objects can use the real model while distant objects use the SDK representation. Camera-driven swapping works in the editor viewport, with hysteresis to reduce boundary flicker.
- Object-library and quick-selection previews use the same High Poly picture provider.

**Terrain, roads, water and lighting**

- Terrain reads the game's heightfield and ground layers; road markings and wear contribute to the map preview.
- Nanite is available for built static geometry, with build-time and rendering tradeoffs.
- Water uses Unreal's water shading with values derived from the level, including color, wave direction, speed and wavelength. Water Lab exposes inspection and tuning controls.
- Water simulation pauses when the water layer is hidden, the scene is cleared or Low Poly mode is active.
- Lighting uses supported authored sun, sky, fog and placed-light data. Vegetation has a wind preview.
- Supported game-mode layouts can be inspected on the same map. Inactive mode variants are excluded from spatial export so their objective IDs do not collide.

**Materials that were missing or wrong**

- Glass transmits the background instead of rendering opaque black.
- Police-sedan liveries composite over the paint using their authored UV layout and transparency.
- Billboard advertisement textures and additional mesh-owned placed-object material bindings are recognized.
- Exact-metadata vegetation, including the reported umbrella and manzanita cases, can use its authored transform when the SDK canopy is too different for a reliable bounds fit.
- Soldier eyes use separate iris and sclera texture bindings.
- Smoke backdrop cards use authored textures and alpha instead of appearing as untextured white rectangles from outside. The known packed rising-smoke sheet has viewport animation.
- Versioned caches invalidate older incomplete material and placed-object records.

**Installation, progress and stability**

- Install from the SDK's map selector. The regular update button checks the matching pair, and New Features displays both sets of notes.
- EA App and Steam installations are supported. Choose the game folder yourself when automatic detection cannot find it.
- The add-on uses the SDK's packaged native reader. No extracted game assets are included in the download.
- Progress includes placed-object replacement, loadout previews and final asset/shader compilation before reporting completion.
- Cancellation reports an incomplete build. Map switches and shutdown wait for or discard work according to the map it belongs to.
- Native-reader access is serialized between geometry and sound decoding; sound workers finish before their context is released.

**Current limits**

- Cold builds can exceed ten seconds. Performance depends on map size, enabled layers and hardware; universal 60 FPS is not established.
- Unsupported vehicle mounts are omitted and reported as incomplete, including a known Abrams driver-camera mount.
- Smoke cards can still show sharp intersections when flown through. Preview animation does not reconstruct the full particle system or exact authored timing.
- Terrain, water and specialized materials retain approximations; coverage varies across maps and assets.
- Configured pickup logic requires mode integration and Portal playtesting. It does not preserve ammunition; the optional conservative watcher requires two distinct known weapons already equipped.

**Detailed fixes and release history**

**Spawner loadouts**

- Select a loot or soldier spawner and press Space, or use the LOADOUT pill, to edit its preview. Loot defaults to an M4A1 aligned to the SDK marker. Soldier spawners show a posed character holding a weapon, with character, outfit, faction and pose choices.
- Available weapon attachments update the preview. Generate a configured weapon card or a TypeScript helper from the same selection; these remain reusable across proximity prompts, buy stations and progression modes.
- Vehicle previews now assemble third-person wheels, tracks, cockpit and mounted weapon parts, retaining repeated instances and resolving supported mounts from vehicle and attachment skeletons.

**Materials and missing objects**

- Glass transmits the background correctly under UE 5.8 instead of becoming opaque black.
- Backdrop smoke cards read their authored texture and alpha. The known packed rising-smoke sheet animates; photographic smoke cards retain their original color.
- Mesh-owned material scopes fill missing placed-object bindings without overriding the placing scope. Blueprint object variations are preserved, including police-sedan liveries.
- Billboard advertisement textures are recognized. Exact-metadata vegetation can use its authored transform when the simplified SDK canopy cannot provide a reliable bounds fit.
- Soldier eye materials read separate iris and sclera textures and normals.
- Material and placed-walk caches are versioned so older incomplete bindings are rebuilt.
- Car-paint wraps composite their livery over the paint through the authored UV layout and transparency.

**Build progress and installation**

- The build includes placed-object dressing, loadout previews and final asset/shader compilation in its progress rather than reporting completion before those stages.
- UI sound reads yield to map geometry under the shared native-reader lock. Sound workers finish before the editor releases their context during shutdown.
- Install from the SDK map selector, update through the regular update button, and read these notes from NEW FEATURES. Requires the matching SDK 0.8.0 release.

**Known limits**

- Cold builds can exceed ten seconds, and a steady 60 FPS depends on the map and hardware.
- Some vehicle mounts are still unsupported, including the Abrams driver camera. The menu reports an incomplete preview and leaves those parts out instead of drawing them at the origin.
- Smoke animation is a preview of the known packed sheet, not the game's full effects simulation. Terrain, water and some specialized materials remain approximations.
- Flying through a smoke card can still expose a sharp intersection at the camera.
- Attachment packages require the generated TypeScript pickup logic to affect gameplay. The conservative watcher refuses ambiguous pickups; replacement does not preserve existing ammunition.

First version numbered alongside the SDK. From here the two move together: a High Poly
release states the SDK it was built against, and the tool will not load a pair it knows to
be incompatible.

**The editor no longer crashes when you play a sound**

- Playing an SFX preview while object previews were still building took the editor down with
  an access violation inside the native core. Two different background paths, the icon
  builder and the sound preview, both tested the same unguarded flag and both entered the
  catalogue mount, so the second one walked state the first was still constructing. C++
  guarantees a function local static is initialised once; it promises nothing about the flag
  being read and written afterwards, which is why the idiom looked safe. Opening the install
  and mounting the catalogue are now serialised, and the second caller waits rather than
  racing. Waiting is the right answer as well as the safe one: it needed the catalogue
  anyway, and a cold mount is the better part of a minute.

**Only the game mode you are building gets exported**

- Objectives built for one mode and then another both went into the spatial export, with
  their object ids colliding, because switching mode only hid the set you were not using and
  the exporter does not look at whether something is visible. A hidden variant now says so
  explicitly and the exporter leaves it out. Hiding an ordinary object to see past it while
  you work still has nothing to do with what gets exported, which is the way round it has to
  be: an export that quietly dropped whatever was hidden would lose real map content.

**One panel instead of a wheel you had to hold open**

- HIGH POLY now opens a panel. The ring was a good way to flip a switch you already knew
  about and a poor way to find out what the add-on was doing: which install it was reading,
  which map, whether the last build worked and what was switched off were spread across a
  dozen pills and a separate status popup, one line at a time. The panel says all of it at
  once, grouped into look, layers, game mode, build, placed objects and previews, water, and
  setup, with the install and the current map at the top.
- Every row is live. A build finishing, a preview count climbing or a worker failing appears
  without reopening anything.
- The ring is still there for anyone who preferred it, on `BF6.HighPoly.Ring`. Both are built
  from one list of controls, so neither can offer something the other does not, and a module
  that adds a switch gets it in both without knowing either exists.
- WATER LAB is reachable from the panel rather than only from a viewport button.

**Switching map clears the scenery that belonged to the last one**

- The add-on only listened for Unreal opening a level file. The SDK's map selector never
  opens one: it keeps a single world and swaps its own contents, so that notification never
  arrived and high poly scenery from the previous map stayed in the new one. It now follows
  the tool's own map lifecycle, and a build that finishes after you have moved on is
  discarded rather than dropped into whichever map is open by then.

**Water stops simulating when nothing is drawing it**

- CLEAR destroyed the scenery but left the ocean simulation, its cascades and every per-map
  raster alive, and the simulation kept stepping thirty times a second whether or not
  anything was on screen. It now pauses when nothing is drawing water, which is any of
  cleared, water layer off, or low poly mode, and says so once when it does. Pausing keeps
  the spectrum and the textures, so turning water back on is a frame rather than a rebuild.

**Cancelling says it cancelled**

- Stopping a build reported the same cheerful summary as a finished one and left a partial
  scene with no sign that anything was missing. Cancelling is now checked at every stage
  rather than only in the object loop, and the result says the build was stopped and that
  the scene is incomplete.

**Shutting down waits for its own workers**

- Closing the editor could unload the add-on while background catalogue and mount workers
  were still inside the native core. Those workers are now joined before the core closes.
- Turning off placed object high poly while a mount was still running left the shared core
  marked busy for the rest of the session, so every later build refused with a message about
  the catalogue still being read. The mount is now completed even when the feature is off.

**Previews agree with each other**

- The quick select radial drew a low poly model on hover while the object library card for
  the same object showed the high poly picture. The library asked the add-on and the radial
  did not; it went straight to the SDK's own mesh. The radial now asks the same add-on, on
  every repaint rather than once, so the picture appears by itself the moment it is rendered
  instead of staying low poly until you hover again. Objects the add-on has no picture for
  keep the live model you can orbit.

**Installed copies can find the native core**

- Three places built the path to bf6_core.dll into the Source tree, which exists on a
  machine that built the plugin and in no copy that installed it. Anybody with an installed
  add-on got a failure naming a path that was never going to be there. One resolver now
  answers for the whole add-on, staged location first, development second, and the game
  mode and Water Lab paths go through it too.

**Changing the game folder means what it says**

- The reader returned early whenever a context was already open, without comparing the
  folder, so switching install left it reading the old one while the disk cache was
  reconfigured for the new one. They then disagreed silently and produced meshes from one
  install keyed against the other. The mismatch is now refused with an explanation rather
  than ignored. Reopening in place is deliberately not attempted while catalogue workers may
  still hold the context.

**Terrain cache**

- A cached terrain record was accepted if its size and sample count were both positive and
  the bytes were present, but nothing checked that the count actually describes a square
  grid, which is how everything downstream reads it. A truncated or half written record
  passed that test and read off the end of the array much later, a long way from the cause.
