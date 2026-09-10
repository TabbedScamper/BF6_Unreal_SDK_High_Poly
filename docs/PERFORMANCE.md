# High Poly performance controls

The Performance choice in the High Poly panel changes viewport lighting, shadows and effects. Balanced uses Unreal's High settings for these groups; Performance uses Medium. Current editor quality restores the settings captured before applying a preset in this session. Texture quality, geometry, authored material bindings, placement identities and Portal exports are unchanged by this choice. These are rendering budgets, not hardware certification.

Runtime material textures now have a mip provider. It stores the authored block-compressed mip chain in session-owned files under the project's Saved/BF6UnrealSDK/HighPoly/Streaming directory and loads requested mips on a streaming worker. This storage supports residency changes even on a fresh derived cache. The files are released with their textures. An interrupted process can leave orphan files; do not remove this directory while an editor is using it. If creating backing storage fails, the existing resident texture path remains available.

The native reader releases a capped texture payload after Unreal copies it, preserving its resource id. Independent reader sessions have separate texture cache identities. A level change also invalidates capped native decodes.

## Testing controls

Apply these before building:

| Command | Purpose |
| --- | --- |
| `BF6.HighPoly.Quality performance` | Medium lighting, shadow and effect groups |
| `BF6.HighPoly.Quality balanced` | High lighting, shadow and effect groups |
| `BF6.HighPoly.Quality current` | Restore the prior editor groups |
| `BF6.HighPoly.TextureStreaming 0` | Resident-texture comparison |
| `BF6.HighPoly.TextureStreaming 1` | Authored mip streaming, the default |
| `BF6.HighPoly.BuildBatch 32` | Limit concurrent mesh preparation inputs |
| `BF6.HighPoly.BuildBatch 0` | Automatic batch size based on installed RAM |
| `BF6.HighPoly.TerrainBatch 16` | Bound simultaneous terrain descriptions, the default (4..256) |
| `BF6.HighPoly.Nanite off` | Conventional-geometry comparison |
| `BF6.HighPoly.Nanite on` | Normal Nanite eligibility |
| `BF6.HighPoly.GameLODs 1` | Experimental authored LOD experiment |
| `BF6.HighPoly.GameLODs 0` | Original single-LOD conventional meshes, the default |
| `BF6.HighPoly.WaterAsync 1` | Compute ocean frames on one worker job, the default |
| `BF6.HighPoly.WaterAsync 0` | Synchronous ocean comparison |
| `BF6.HighPoly.CompactVertices 1` | Share exactly matching render corners, the default |
| `BF6.HighPoly.CompactVertices 0` | Original unshared render-buffer comparison |

The authored LOD experiment applies to conventional base-map props with at least 512 triangles. It preserves LOD 0 and adds up to two smaller authored LODs when section materials match. It does not apply to Nanite, terrain receivers, glass/decal sections, placed-object assemblies or loadout previews. Incompatible material layouts keep their original representation. Additional LODs consume construction time and memory, so compare both load and frame results before enabling them broadly.

Mesh batches limit temporary decoded data. Optional derived writes retain at most 128 MiB of queued raw input; writes beyond that budget are skipped, leaving a future cache miss. This does not omit an asset from the scene. Rebuilds collect destroyed preview objects before allocating their replacements.

Terrain descriptions are prepared and committed in batches instead of retaining every tile's description at once. Grid resolution and generated triangles remain unchanged. Water tile transforms and shader width data are submitted together before building the instance hierarchy, avoiding an immediately invalidated automatic build.

Conventional render meshes share corners only when their source vertex, normal, tangent, handedness, all UV channels and color match exactly. Triangles, material sections and authored seams remain intact. Nanite meshes keep their existing build path. State snapshots record the before/after corner counts for the primary conventional LODs.

Ocean computation uses copied numeric inputs and publishes completed results on the editor thread. There is at most one pending job, so slow machines keep the latest completed ocean frame instead of accumulating work. Reset drains the worker before releasing its owner. Deterministic tests compare displacement, normal pixels and foam history against the synchronous calculation; this change alone did not materially improve the Dumbo flight benchmark.

New transient materials prepare their properties through Unreal's standard path and compile with a material update context that synchronizes rendering. They no longer request a global scene render-state recreation merely to create an unbound parent. Existing material edits are outside this helper's scope.

Build finalization also queues incomplete parent shader maps for the active quality before waiting for compilation. A preset change can invalidate startup-prepared variants without immediately submitting replacement jobs. Automated readiness checks include shader-map completeness, rather than only the pending job count.

`BF6.HighPoly.PerfCapture start <csv-path>` and `BF6.HighPoly.PerfCapture stop` record the engine's game, render, RHI and GPU timing counters at frame boundaries. Counters can be delayed or represent the last viewport draw. A GPU value of zero means unavailable. They are diagnostic counters, not a substitute for presented-frame capture or a GPU trace.

Use the SDK's `Tools/Test-LowSpec.ps1` and `Tools/stability/matrix.py` for repeatable scene opens, camera motion, unloads and export/save checks. The current automated budgets do not certify 60 FPS, a ten-second cold open, or operation on a particular 6 GB graphics card. Check the individual reports and their measurement limits.

Closing during a game-mode read now waits for the active native reader before unloading its DLL. The previous ten-second timeout could leave code executing in an unloaded module. Queued reads are cancelled before starting once shutdown begins. An already-active read currently has to finish, so closing immediately after opening a map can take longer; cooperative native cancellation remains future work.
