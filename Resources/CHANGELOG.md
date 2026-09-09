# BF6 High Poly version history

## 0.8.0 (2026-09-09)

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
