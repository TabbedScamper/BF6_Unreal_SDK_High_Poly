"""The whole scene, through the real reader, with this map's archives only.

Answers three things the editor cannot be asked quickly:
  1. how many of the scene's types resolve, and through which tier;
  2. what the resolve pass actually costs, per type and in total;
  3. what the light walk costs on top, which is the part that is now skipped.

Read-only. Never calls bf6_mount_all.
"""
import ctypes as c, json, sys, time
from pathlib import Path

PROJECT = Path(r'C:\Users\mwalt\Documents\Unreal Projects\BF6_High_Poly')
DLL = PROJECT / 'Plugins/BF6UnrealSDK/Source/ThirdParty/libbf6/bin/Win64/bf6_core.dll'
GAME = (PROJECT / 'Saved/BF6UnrealSDK/HighPoly/install.txt').read_text().strip()
SCENE = PROJECT / 'Saved/BF6UnrealSDK/saves/experiences/UNDEAD GROUND ZERO GUNMASTER V1.4/maps/MP_Aftermath/MP_Aftermath.json'
LEVEL = 'MP_Aftermath'
LIMIT = int(sys.argv[1]) if len(sys.argv) > 1 else 0     # 0 = every type

lib = c.CDLL(str(DLL))
def bind(name, args, ret=c.c_int):
    f = getattr(lib, name); f.argtypes = args; f.restype = ret; return f
op        = bind('bf6_open', [c.c_char_p, c.c_void_p, c.c_int], c.c_void_p)
close     = bind('bf6_close', [c.c_void_p], None)
mount     = bind('bf6_mount_level_archives', [c.c_void_p, c.c_char_p, c.c_void_p, c.c_int])
instances = bind('bf6_asset_instances', [c.c_void_p, c.c_char_p, c.c_void_p, c.c_int, c.c_void_p, c.c_int])
lights    = bind('bf6_asset_lights', [c.c_void_p, c.c_char_p, c.c_void_p, c.c_int, c.c_void_p, c.c_void_p, c.c_int])

class Asset(c.Structure):
    _fields_ = [('name', c.c_char_p), ('type', c.c_uint32), ('size', c.c_uint32)]
list_ebx  = bind('bf6_list_ebx', [c.c_void_p, c.c_char_p, c.POINTER(Asset), c.c_int])
err = c.create_string_buffer(2048)

ART = ['com_', 'mil_', 'fed_', 'naf_', 'cas_', 'ind_', 'psd_', 'ter_', 'veg_', 'ob_']

def tiers(key):
    k = key.lower()
    exact = ['pf_portal_' + k, k, 'pf_portal_' + k + '_a']
    for p in ART:
        exact += ['pf_portal_' + p + k, 'pf_portal_' + p + k + '_a']
    base, letter = k, False
    if len(k) > 2 and k[-2] == '_' and k[-1].isalpha():
        base, letter = k[:-2], True
    alias = []
    if letter:
        alias += ['pf_portal_' + base, 'pf_portal_br_' + base]
    alias.append('pf_portal_br_' + k)
    if letter:
        alias += ['pf_portal_' + p + base for p in ART]
    for p in ART:
        alias.append('pf_portal_br_' + p + k)
        if letter:
            alias.append('pf_portal_br_' + p + base)
    return exact, alias

scene = json.loads(SCENE.read_text(encoding='utf-8'))
counts = {}
for r in scene.get('objects', []):
    key = (r.get('label') or r.get('mesh') or '').strip()
    if key:
        counts[key] = counts.get(key, 0) + 1
types = sorted(counts, key=lambda k: -counts[k])
if LIMIT:
    types = types[:LIMIT]

print(f'{len(counts)} distinct types over {sum(counts.values())} objects; probing {len(types)}', flush=True)

report = {'level': LEVEL, 'types': {}, 'objects': sum(counts.values())}
ctx = None
try:
    t0 = time.perf_counter()
    ctx = op(GAME.encode(), err, len(err))
    report['open_seconds'] = round(time.perf_counter() - t0, 3)
    if not ctx:
        raise SystemExit('bf6_open failed: ' + err.value.decode(errors='replace'))
    t0 = time.perf_counter()
    if mount(ctx, LEVEL.encode(), err, len(err)) != 1:
        raise SystemExit('mount failed: ' + err.value.decode(errors='replace'))
    report['mount_seconds'] = round(time.perf_counter() - t0, 3)
    print(f"open {report['open_seconds']}s, mount {report['mount_seconds']}s", flush=True)

    # THE NAME INDEX, WHICH IS WHAT MAKES A CANDIDATE LIST FREE.
    #
    # The plugin builds this once (EnsurePrefabIndex) and then a candidate that
    # does not exist is a hash lookup rather than a query. Without it every miss
    # costs about 0.6 s, which is what makes a long candidate list look
    # ruinous - and is why the alias tier is safe to add.
    t0 = time.perf_counter()
    n = list_ebx(ctx, b'pf_portal_', None, 0)
    rows = (Asset * max(n, 0))()
    got = list_ebx(ctx, b'pf_portal_', rows, n) if n > 0 else 0
    index = set()
    for r in rows[:max(got, 0)]:
        nm = (r.name or b'').decode(errors='replace')
        index.add(nm.rsplit('/', 1)[-1])
    report['index_seconds'] = round(time.perf_counter() - t0, 3)
    report['index_names'] = len(index)
    print(f"prefab index: {len(index)} names in {report['index_seconds']}s", flush=True)

    totals = {'exact': 0, 'alias': 0, 'unresolved': 0}
    resolve_secs = light_secs = 0.0
    obj_ok = obj_alias = obj_no = 0
    started = time.perf_counter()

    for i, key in enumerate(types):
        exact, alias = tiers(key)
        row = {'objects': counts[key], 'tier': None, 'name': None, 'members': 0}
        t1 = time.perf_counter()
        tried = 0
        for tier, names in (('exact', exact), ('alias', alias)):
            for nm in names:
                # The index answers for every pf_portal_ candidate, as the
                # plugin does. Only a name that exists is walked.
                if nm.startswith('pf_portal_') and nm not in index:
                    continue
                tried += 1
                err.value = b''
                n = instances(ctx, nm.encode(), None, 0, err, len(err))
                if n > 0:
                    row.update(tier=tier, name=nm, members=n)
                    break
            if row['tier']:
                break
        row['walks'] = tried
        row['resolve_seconds'] = round(time.perf_counter() - t1, 3)
        resolve_secs += row['resolve_seconds']

        if row['tier']:
            t2 = time.perf_counter()
            err.value = b''
            row['lights'] = lights(ctx, row['name'].encode(), None, 0, None, err, len(err))
            row['light_seconds'] = round(time.perf_counter() - t2, 3)
            light_secs += row['light_seconds']
            totals[row['tier']] += 1
            if row['tier'] == 'exact':
                obj_ok += counts[key]
            else:
                obj_alias += counts[key]
        else:
            totals['unresolved'] += 1
            obj_no += counts[key]

        report['types'][key] = row
        if (i + 1) % 20 == 0:
            print(f'  {i+1}/{len(types)}  resolve {resolve_secs:.1f}s  lights {light_secs:.1f}s', flush=True)

    report['summary'] = {
        'types_exact': totals['exact'], 'types_alias': totals['alias'],
        'types_unresolved': totals['unresolved'],
        'objects_exact': obj_ok, 'objects_alias': obj_alias, 'objects_unresolved': obj_no,
        'resolve_seconds': round(resolve_secs, 1),
        'light_seconds': round(light_secs, 1),
        'wall_seconds': round(time.perf_counter() - started, 1),
    }
    print(json.dumps(report['summary'], indent=2))
finally:
    if ctx:
        close(ctx)
    Path(__file__).with_name('scene-probe.json').write_text(json.dumps(report, indent=2))
