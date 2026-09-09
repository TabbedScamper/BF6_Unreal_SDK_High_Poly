# Measuring the placed-object path without opening Unreal

`scene-resolve-probe.py` loads the same `bf6_core.dll` the add-on uses, opens the
install the add-on has saved, mounts **only the current map's archives**, and
runs every type in a saved scene through the same candidate order the resolver
uses. It never calls `bf6_mount_all` and never writes anything.

```
python scene-resolve-probe.py          # every type in the scene
python scene-resolve-probe.py 12       # the twelve commonest, for a quick look
```

It writes `scene-probe.json` beside itself and prints a summary.

## What it is for

The editor cannot answer "why is this slow" quickly: a run takes minutes and the
answer is buried in a log. This answers three questions in one pass, against the
real reader:

- **Does every type in the scene resolve, and through which tier?** Exact name,
  alias, or not at all.
- **What does resolving cost?** Per type and in total.
- **What do the light walks cost?** Separately, because they are optional and
  are now skipped unless the lights are actually wanted.

## Measured on MP_Aftermath, Undead Ground Zero, 8 September 2026

3,502 placed objects across 192 types, this map's archives only:

| | types | objects |
|---|---:|---:|
| resolved by exact name | 176 | 2,903 |
| resolved by alias | 6 | 458 |
| no prefab at all | 10 | 141 |

Of those last 141, all but one are spawners, triggers, areas and cameras, which
are gameplay logic and have no art in the game by design. The exception is a
single vehicle.

| stage | seconds |
|---|---:|
| open the install | 1.1 |
| mount this map | 5.4 |
| build the prefab name index | 0.06 |
| resolve all 192 types | 147.9 |
| walk their lights | 116.8 |

The light walks are 44% of the reader time and find nothing for most props: 0.6
seconds each to discover that a wall has no fixtures. That is why they are no
longer done during the resolve.

The name index is what makes a long candidate list free: 1,725 names in 57
milliseconds, after which a candidate that does not exist costs a hash lookup
rather than a 0.6 second query. Across all 192 types the resolver performed 211
walks in total, so the alias tier adds almost nothing.

## Editing it

The candidate order in `tiers()` mirrors `FindPrefab` in
`BF6HighPolyPlaced.cpp`. If you change one, change the other, or the probe stops
being evidence about the tool.
