#!/usr/bin/env python3
"""The actor list for the spawner (runtime/actor_spawn.c): every actor of the player's own RomFS, in spawn-tab categories,
with a readable name. Game data: written to the build folder only, never committed.

Actors: Pack/Actor/<name>.pack.zs (what the game can create), with RSDB/ActorInfo (Category) and RSDB/PouchActorInfo
(PouchCategory: items that go into the inventory) for the category. Names, in this order (the "source" column):
  game        the game's own English name: <actor>_Name in Mals/USen.*.sarc.zs ActorMsg/*.msbt (EUen if USen lacks it)
  lookup      names/actor_names.tsv next to this script, our own table for actors the message files do not name
  deciphered  rules: a capsule or bundle named after its device, a variant after its named base actor, enemy tiers and
              families, and a word table for the internal words (Zonau -> Zonai, Golem -> Talus)
  internal    none of these: the internal name made readable (CamelCase and underscores split); mostly level geometry,
              locators and test actors
Categories (the spawn tab's menus): creatures (enemies, animals), npcs, items (materials, food, key items, arrows, rupees),
weapons (weapons, bows, shields), armor, zonai (Zonai devices and physics objects), objects (other world objects) and
system (locators, sound, camera, player parts: hidden by the tab). spawn: "pickup" for an actor the inventory takes (it
appears on the ground and is picked up), "world" for everything else; the game creates both with the same call.
dishes: the cooked meals (Item_Cook_*, PouchActorInfo Food), which have no actor pack: "pouch", given into the inventory
(runtime/dish_give.h). After the actors, one "@effect<TAB>name<TAB>min level<TAB>max level<TAB>base seconds" line per
meal effect of the game's cooking table (Cooking/CookingTable SystemData EffectList; LifeRecover is the hearts field).

Output build/<version>/actor_list.tsv: '#' comment lines (counts), then name<TAB>category<TAB>spawn<TAB>display<TAB>source,
sorted by category then display name.
usage: prepare_actor_list.py [--version v143] [--root <tree with extracted/<version>/romfs>] [--out <file>]
"""
import argparse, collections, json, re, struct, sys
from pathlib import Path
import compression.zstd as zstd

ROOT = Path(__file__).resolve().parents[1]
CATEGORIES = ('creatures', 'npcs', 'items', 'dishes', 'weapons', 'armor', 'zonai', 'objects', 'system')
SOURCES = ('game', 'lookup', 'deciphered', 'internal')


# ---- containers -------------------------------------------------------------------------------------------------
def sarc(data):
    """{member name: bytes} of a SARC archive."""
    if data[:4] != b'SARC': raise ValueError('not a SARC archive')
    e = '<' if data[6:8] == b'\xff\xfe' else '>'
    data_off = struct.unpack_from(e + 'I', data, 0x0c)[0]
    sfat = struct.unpack_from(e + 'H', data, 4)[0]
    count = struct.unpack_from(e + 'H', data, sfat + 6)[0]
    names = sfat + 12 + count * 16 + 8
    out = {}
    for i in range(count):
        _, attr, start, end = struct.unpack_from(e + 'IIII', data, sfat + 12 + i * 16)
        at = names + (attr & 0xffffff) * 4
        out[data[at:data.index(b'\0', at)].decode()] = data[data_off + start:data_off + end]
    return out


def msbt(data):
    """{label: text} of an MSBT message file, control tags removed."""
    if data[:8] != b'MsgStdBn': raise ValueError('not an MSBT file')
    e = '<' if data[8:10] == b'\xff\xfe' else '>'
    sections, at = struct.unpack_from(e + 'H', data, 0x0e)[0], 0x20
    labels, texts = {}, []
    for _ in range(sections):
        magic, size = data[at:at + 4], struct.unpack_from(e + 'I', data, at + 4)[0]
        body = at + 0x10
        if magic == b'LBL1':
            for i in range(struct.unpack_from(e + 'I', data, body)[0]):
                count, off = struct.unpack_from(e + 'II', data, body + 4 + i * 8)
                p = body + off
                for _ in range(count):
                    n = data[p]
                    labels[data[p + 1:p + 1 + n].decode()] = struct.unpack_from(e + 'I', data, p + 1 + n)[0]
                    p += 1 + n + 4
        elif magic == b'TXT2':
            n = struct.unpack_from(e + 'I', data, body)[0]
            offs = [struct.unpack_from(e + 'I', data, body + 4 + i * 4)[0] for i in range(n)] + [size]
            texts = [msbt_text(data[body + offs[i]:body + offs[i + 1]], e) for i in range(n)]
        at = body + ((size + 15) & ~15)
    return {k: texts[v] for k, v in labels.items() if v < len(texts)}


def msbt_text(raw, e):
    out, i = [], 0
    while i + 1 < len(raw):
        c = struct.unpack_from(e + 'H', raw, i)[0]; i += 2
        if c == 0: break
        if c == 0x0e: i += 6 + struct.unpack_from(e + 'H', raw, i + 4)[0]; continue   # tag: group, type, size, data
        if c == 0x0f: i += 4; continue                                                  # closing tag
        out.append(chr(c))
    return ''.join(out).strip()


def byml(data):
    """A BYML document (versions 2-7) as Python values; hash-keyed dictionaries keep their integer keys."""
    e = '<' if data[:2] == b'YB' else '>'
    _, keys_off, strings_off, root = struct.unpack_from(e + 'HIII', data, 2)
    u32 = lambda o: struct.unpack_from(e + 'I', data, o)[0]

    def table(o):
        if not o: return []
        starts = [u32(o + 4 + 4 * i) for i in range(u32(o) >> 8)]
        return [data[o + s:data.index(b'\0', o + s)].decode('utf-8', 'replace') for s in starts]
    keys, strings = table(keys_off), table(strings_off)

    def value(kind, raw):
        if kind == 0xa0: return strings[raw]
        if kind in (0xc0, 0xc1, 0x20, 0x21): return node(raw)
        if kind == 0xd0: return bool(raw)
        if kind == 0xd1: return struct.unpack(e + 'i', struct.pack(e + 'I', raw))[0]
        if kind == 0xd2: return struct.unpack(e + 'f', struct.pack(e + 'I', raw))[0]
        if kind == 0xd3: return raw
        if kind in (0xd4, 0xd5): return struct.unpack_from(e + ('q' if kind == 0xd4 else 'Q'), data, raw)[0]
        if kind == 0xd6: return struct.unpack_from(e + 'd', data, raw)[0]
        if kind == 0xa1: return data[raw + 4:raw + 4 + u32(raw)]
        return None

    def node(o):
        kind, n = data[o], u32(o) >> 8
        if kind == 0xc0:
            base = o + 4 + ((n + 3) & ~3)
            return [value(data[o + 4 + i], u32(base + 4 * i)) for i in range(n)]
        if kind == 0xc1:
            return {keys[u32(o + 4 + 8 * i) & 0xffffff]: value(data[o + 4 + 8 * i + 3], u32(o + 8 + 8 * i)) for i in range(n)}
        if kind in (0x20, 0x21):
            width = 4 if kind == 0x20 else 8
            types = o + 4 + n * (width + 4)
            return {struct.unpack_from(e + ('I' if width == 4 else 'Q'), data, o + 4 + (width + 4) * i)[0]:
                    value(data[types + i], u32(o + 4 + (width + 4) * i + width)) for i in range(n)}
        return None
    return node(root) if root else None


class RomFS:
    """Files of an extracted RomFS image (extracted/<version>/romfs: index.json + romfs.bin), .zs with the game's
    dictionaries (Pack/ZsDic.pack.zs)."""
    def __init__(self, folder):
        manifest = json.loads((folder / 'index.json').read_text())
        self.files = {f['path']: f for f in manifest['files']}
        self.image = (folder / 'romfs.bin').open('rb')
        self.dicts = None

    def raw(self, path):
        f = self.files[path]; self.image.seek(f['offset']); data = self.image.read(f['size'])
        if len(data) != f['size']: raise OSError(f'short read: {path}')
        return data

    def read(self, path):
        data = self.raw(path)
        if not path.endswith('.zs'): return data
        try: return zstd.decompress(data)
        except zstd.ZstdError: pass
        if self.dicts is None:
            self.dicts = [zstd.ZstdDict(d) for d in sarc(zstd.decompress(self.raw('Pack/ZsDic.pack.zs'))).values()]
        for d in self.dicts:
            try: return zstd.decompress(data, zstd_dict=d)
            except zstd.ZstdError: continue
        raise ValueError(f'cannot decompress {path}')

    def first(self, pattern):
        """The newest file whose path matches the regular expression (product files carry the version in the name)."""
        hits = sorted(p for p in self.files if re.fullmatch(pattern, p))
        return hits[-1] if hits else None


# ---- names ------------------------------------------------------------------------------------------------------
def game_names(romfs):
    """({actor: English name} from the ActorMsg message files (USen, then EUen for what USen lacks), the game's English
    vocabulary: every word of every English message, lower case)."""
    names, vocabulary = {}, set()
    for region in ('USen', 'EUen'):
        path = romfs.first(rf'Mals/{region}\.Product\.\d+\.sarc\.zs')
        if not path: continue
        for member, data in sorted(sarc(romfs.read(path)).items()):
            if not member.endswith('.msbt'): continue
            texts = msbt(data)
            for text in texts.values(): vocabulary.update(w.lower() for w in re.findall(r'[A-Za-z]+', text))
            if not member.startswith('ActorMsg/'): continue
            for label, text in texts.items():
                if label.endswith('_Name') and text and not text.startswith('-'):
                    names.setdefault(label[:-5], text)
    return names, vocabulary


def lookup_names(path):
    """{actor: name} from our own table (name<TAB>display, '#' comments); missing file: none."""
    out = {}
    if path.is_file():
        for line in path.read_text().splitlines():
            if line.strip() and not line.startswith('#'):
                actor, _, display = line.partition('\t')
                if display.strip(): out[actor.strip()] = display.strip()
    return out


# Words of internal names and the game's English for them (enemy and family names from the in-game compendium).
WORDS = {
    'Zonau': 'Zonai', 'Golem': 'Talus', 'Mogurudo': 'Frox', 'Assassin': 'Yiga', 'Giant': 'Hinox', 'Hatago': 'Stable',
    'Lynel': 'Lynel', 'Bokoblin': 'Bokoblin', 'Moblin': 'Moblin', 'Lizalfos': 'Lizalfos', 'Horriblin': 'Horriblin',
    'Keese': 'Keese', 'Chuchu': 'Chuchu', 'Octarock': 'Octorok', 'Likelike': 'Like Like', 'LikeLike': 'Like Like',
    'Gibdo': 'Gibdo', 'Wizzrobe': 'Wizzrobe', 'Drake': 'Gleeok', 'Kohga': 'Master Kohga', 'Ganondorf': 'Ganondorf',
    'Zelda': 'Zelda', 'Sage': 'Sage', 'Korok': 'Korok', 'Kokko': 'Cucco', 'Horse': 'Horse', 'Ganon': 'Ganon',
    'Rupee': 'Rupee', 'TBox': 'Treasure Chest', 'RBox': 'Rock Box', 'Hyrule': 'Hyrule', 'Hylia': 'Hylian',
    'Goron': 'Goron', 'Rito': 'Rito', 'Zora': 'Zora', 'Gerudo': 'Gerudo', 'Sheikah': 'Sheikah', 'Miasma': 'Gloom',
    'Dgn': 'Shrine', 'Cave': 'Cave', 'Well': 'Well', 'Sky': 'Sky', 'Minus': 'Depths', 'Fld': 'Field', 'Twn': 'Town',
    'Obj': 'Object', 'Npc': 'NPC', 'Asb': 'Assembled', 'Mrg': 'Merged', 'Sp': 'Special', 'Pic': 'Picture', 'Ef': 'Effect',
    'Eft': 'Effect', 'Loc': 'Locator', 'Dm': 'Demo', 'Tmp': 'Temporary', 'Sheiker': 'Sheikah', 'Kibako': 'Wooden Box',
    'Labo': 'Lab', 'Tsubo': 'Pot', 'Sunazarashi': 'Sand Seal', 'Nushi': 'Lord of the Mountain', 'GameRomHorse': 'Horse',
    'Lsword': 'Two-Handed Sword', 'Moriblin': 'Moblin', 'Horablin': 'Horriblin', 'Toby': 'Aerocuda', 'Treant': 'Evermean',
    'Sandworm': 'Molduga', 'Zombie': 'Gibdo', 'Kaeru': 'Frog', 'Inobuta': 'Boar', 'Uma': 'Horse', 'Kinoko': 'Mushroom',
    'Sakana': 'Fish', 'Tori': 'Bird', 'Ishi': 'Stone', 'Iwa': 'Rock', 'Taru': 'Barrel', 'Hako': 'Box', 'Ki': 'Tree',
    'Hashira': 'Pillar', 'Kusa': 'Grass', 'Mizu': 'Water', 'Yuka': 'Floor', 'Kabe': 'Wall', 'Tobira': 'Door',
}
# Enemy tiers as the compendium names them (Enemy_Bokoblin_Junior: Bokoblin, _Middle: Blue Bokoblin, ...).
TIERS = {'Junior': '', 'Middle': 'Blue', 'Senior': 'Black', 'Dark': 'Silver', 'Gold': 'Gold'}
PREFIX_DROP = {'FldObj', 'TwnObj', 'DgnObj', 'SkyObj', 'Obj', 'SpObj', 'MinusObj', 'CaveObj', 'AsbObj', 'Item', 'Npc',
               'Enemy', 'Animal', 'Weapon', 'Armor', 'PicObj', 'OctObj', 'EftLoc', 'MrgPrefab'}
VARIANT = re.compile(r'^(?:[A-Z]|\d+|[A-Z]\d+|\d+[A-Z]?|[A-Z]{1,2}\d*)$')   # A, 01, A01, 1B, LL: variant codes


def split_words(token):
    return re.findall(r'[A-Z]+(?=[A-Z][a-z]|\d|$)|[A-Z]?[a-z]+|\d+|[A-Z]+', token)


def readable(name):
    """(display, words): the internal name as words (prefix dropped, the word table applied, variant codes in
    parentheses), and the words themselves."""
    parts = name.split('_')
    if len(parts) > 1 and parts[0] in PREFIX_DROP: parts = parts[1:]
    words, variant = [], []
    for part in parts:
        if VARIANT.match(part) and words: variant.append(part); continue
        if part in WORDS: words.append(WORDS[part]); continue
        words.extend(WORDS.get(w, w) for w in split_words(part) or [part])
    text = ' '.join(words) or name
    if variant: text += f" ({' '.join(variant)})"
    return text, words


TIER_FAMILY = {'Chuchu': {'Middle': 'Large', 'Senior': 'Huge'}}   # where the compendium words a tier differently
PARTS = {'Head': 'head', 'Upper': 'body', 'Lower': 'legs'}


class Decipher:
    """Names from rules for actors the game and the lookup table leave unnamed. named: {actor: name}."""
    def __init__(self, named, vocabulary=()):
        self.named, self.vocabulary = named, set(vocabulary)
        self.siblings = collections.defaultdict(list)   # every part but the last -> named actors that differ only there
        for actor in sorted(named):
            parts = actor.split('_')
            if len(parts) > 1: self.siblings[tuple(parts[:-1])].append((parts[-1], actor))

    def __call__(self, name):
        named, parts = self.named, name.split('_')
        m = re.fullmatch(r'(SpObj_\w+?)_Capsule(_\w+?)?(?:_Bundle_([A-Z]))?', name)
        if m:   # a Zonai device capsule (and a bundle of several) carries its device's name
            device = named.get(m[1] + (m[2] or '')) or named.get(m[1] + '_A_01')
            if device: return f'{device} Capsule' + (f' (bundle {m[3]})' if m[3] else '')
        m = re.fullmatch(r'(?:[A-Za-z]+_)?Figure_((?:Enemy|Npc|Animal)_\w+?)(_Head)?', name)
        if m:   # a figure (the statuettes) of a named character
            who = named.get(m[1]) or self(m[1])
            if who: return f'{who} Figure' + (' (head)' if m[2] else '')
        tier = next((i for i, p in enumerate(parts) if p in TIERS and p != 'Junior'), None)
        if parts[0] == 'Enemy' and tier is not None:   # a tier of a named enemy: Enemy_X_Middle from Enemy_X_Junior
            base = self.relative(parts[:tier] + ['Junior'] + parts[tier + 1:])
            if base:
                family = parts[1]
                word = TIER_FAMILY.get(family, TIERS).get(parts[tier], TIERS[parts[tier]])
                text, extra = base
                return ' '.join(x for x in (word, text) if x) + (f' ({extra})' if extra else '')
        base = self.relative(parts, keep_all=False)
        if base: return base[0] + (f' ({base[1]})' if base[1] else '')
        m = re.fullmatch(r'Armor_(\d+)_(Head|Upper|Lower)', name)
        if m:   # an unnamed piece of a set whose other piece is named
            other = next((named[f'Armor_{m[1]}_{p}'] for p in PARTS if f'Armor_{m[1]}_{p}' in named), None)
            if other: return f'{other} set ({PARTS[m[2]]})'
        if len(parts) > 2 and VARIANT.match(parts[-1]):   # another variant of a named actor: Animal_Dog_B from _A
            sib = self.siblings.get(tuple(parts[:-1]))
            if sib: return f'{named[sib[0][1]]} ({parts[-1]})'
        # the words themselves, when each one is English the game itself uses (or a word table entry)
        text, words = readable(name)
        known = set(w.lower() for v in WORDS.values() for w in v.split())
        letters = [w for word in words for w in re.findall(r'[A-Za-z]+', word)]
        if letters and all(w.lower() in self.vocabulary or w.lower() in known for w in letters if len(w) > 1): return text
        return None

    def relative(self, parts, keep_all=True):
        """(name of the closest named actor, the words left out) for the actor of these parts: itself, or with one
        part and/or a tail left out (keeping the prefix and the next part); None when none is named."""
        named = self.named
        whole = '_'.join(parts)
        if keep_all and whole in named: return named[whole], ''
        best = None
        n = len(parts)
        for cut in range(n, 1, -1):          # keep parts[:cut] ...
            for drop in [None] + list(range(2, cut)):   # ... minus at most one inner part
                if cut == n and drop is None: continue
                kept = [p for i, p in enumerate(parts[:cut]) if i != drop]
                actor = '_'.join(kept)
                if actor in named and (best is None or len(kept) > best[0]):
                    left = ([parts[drop]] if drop is not None else []) + parts[cut:]
                    best = (len(kept), named[actor], readable('_'.join(left))[0] if left else '')
        return best[1:] if best else None


# ---- categories -------------------------------------------------------------------------------------------------
SYSTEM = {'System', 'Camera', 'EffectLocater', 'SoundLocater', 'SpotBgm', 'AreaBgm', 'GameBalanceLocator', 'Sheath',
          'WeakPoint', 'Player'}
POUCH = {'Weapon': 'weapons', 'Bow': 'weapons', 'Shield': 'weapons', 'Armor': 'armor', 'SpecialParts': 'zonai',
         'Material': 'items', 'Food': 'items', 'KeyItem': 'items', 'Arrow': 'items', 'Rupee': 'items', 'SpecialPower': 'items'}
ACTOR = {'Weapon': 'weapons', 'Bow': 'weapons', 'Shield': 'weapons', 'Armor': 'armor', 'NonPlayerArmor': 'armor',
         'Enemy': 'creatures', 'Animal': 'creatures', 'NPC': 'npcs', 'EventNPC': 'npcs', 'ObjectPouchContent': 'items',
         'ObjectPouchContentActive': 'items', 'ObjectGeneral': 'zonai', 'ObjectGeneralActive': 'zonai',
         'CapturableActor': 'zonai', 'Tool': 'zonai'}


def category(name, actor_category, pouch_category):
    if actor_category in SYSTEM: return 'system'
    if pouch_category in POUCH: return POUCH[pouch_category]
    if name.startswith('SpObj_'): return 'zonai'
    if name.startswith(('Enemy_', 'Animal_')) and actor_category in (None, 'Enemy', 'Animal'): return 'creatures'
    if name.startswith('Npc_') and actor_category in (None, 'NPC', 'EventNPC'): return 'npcs'
    return ACTOR.get(actor_category, 'objects')


def build(romfs, lookup):
    actors = sorted(p[len('Pack/Actor/'):-len('.pack.zs')] for p in romfs.files
                    if p.startswith('Pack/Actor/') and p.endswith('.pack.zs'))
    info_path = romfs.first(r'RSDB/ActorInfo\.Product\.\d+\.rstbl\.byml\.zs')
    pouch_path = romfs.first(r'RSDB/PouchActorInfo\.Product\.\d+\.rstbl\.byml\.zs')
    info = {row.get('__RowId'): row for row in (byml(romfs.read(info_path)) if info_path else [])}
    pouch = {row.get('__RowId'): row for row in (byml(romfs.read(pouch_path)) if pouch_path else [])}
    game, vocabulary = game_names(romfs)
    named = {**lookup, **game}   # the game's own names win
    decipher = Decipher(named, vocabulary)
    rows = []
    for name in actors:
        actor_category = info.get(name, {}).get('Category')
        pouch_category = pouch.get(name, {}).get('PouchCategory')
        cat = category(name, actor_category, pouch_category)
        spawn = 'pickup' if name in pouch else 'world'
        if name in game: display, source = game[name], 'game'
        elif name in lookup: display, source = lookup[name], 'lookup'
        else:
            display = decipher(name)
            source = 'deciphered' if display else 'internal'
            if not display: display = readable(name)[0]
        rows.append((name, cat, spawn, display.replace('\t', ' ').replace('\n', ' '), source))
    have = set(actors)
    for name, row in sorted(pouch.items()):   # dishes: inventory only, no actor pack
        if not name or name in have or not name.startswith('Item_Cook_') or row.get('PouchCategory') != 'Food': continue
        if name in game: display, source = game[name], 'game'
        elif name in lookup: display, source = lookup[name], 'lookup'
        else: display, source = readable(name)[0], 'internal'
        rows.append((name, 'dishes', 'pouch', display.replace('\t', ' ').replace('\n', ' '), source))
    rows.sort(key=lambda r: (CATEGORIES.index(r[1]), r[3].lower(), r[0]))
    return rows


def meal_effects(romfs):
    """(name, min level, max level, base seconds) of the cooking table's meal effects, LifeRecover left out."""
    path = romfs.first(r'Cooking/CookingTable\.game__cooking__Table\.bgyml(\.zs)?')
    if not path: return []
    table = byml(romfs.read(path))
    out = []
    for e in (table.get('SystemData') or {}).get('EffectList') or []:
        name = e.get('Name')
        if isinstance(name, str) and name != 'LifeRecover' and re.fullmatch(r'[A-Za-z0-9_]+', name):
            out.append((name, int(e.get('MinLv', 1)), int(e.get('MaxLv', 1)), int(e.get('BaseTime', 0))))
    return out


def summary(rows):
    counts = collections.Counter((r[1], r[4]) for r in rows)
    spawn = collections.Counter((r[1], r[2]) for r in rows)
    lines = []
    for cat in CATEGORIES:
        total = sum(counts[cat, s] for s in SOURCES)
        lines.append(f'{cat}: {total} (' + ', '.join(f'{s} {counts[cat, s]}' for s in SOURCES) +
                     f'; pickup {spawn[cat, "pickup"]}, world {spawn[cat, "world"]}, pouch {spawn[cat, "pouch"]})')
    lines.append(f'total: {len(rows)} (' + ', '.join(f'{s} {sum(counts[c, s] for c in CATEGORIES)}' for s in SOURCES) + ')')
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', choices=('v0', 'v112', 'v121', 'v143'), default='v143')
    ap.add_argument('--root', type=Path, default=ROOT, help='tree with extracted/<version>/romfs')
    ap.add_argument('--out', type=Path, help='default: <root>/build/<version>/actor_list.tsv')
    args = ap.parse_args()
    romfs = RomFS(args.root / f'extracted/{args.version}/romfs')
    rows = build(romfs, lookup_names(Path(__file__).with_name('names') / 'actor_names.tsv'))
    out = args.out or args.root / f'build/{args.version}/actor_list.tsv'
    out.parent.mkdir(parents=True, exist_ok=True)
    lines = summary(rows)
    text = ''.join(f'# {line}\n' for line in [f'actor list for the spawner ({args.version}), scripts/prepare_actor_list.py'] + lines)
    text += ''.join('\t'.join(r) + '\n' for r in rows)
    text += ''.join(f'@effect\t{n}\t{lo}\t{hi}\t{t}\n' for n, lo, hi, t in meal_effects(romfs))
    tmp = out.with_name(out.name + '.tmp'); tmp.write_text(text); tmp.replace(out)
    print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main())
