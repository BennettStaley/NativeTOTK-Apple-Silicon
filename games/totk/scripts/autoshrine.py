#!/usr/bin/env python3
"""Auto shrine, driven from outside the game (development tool): the flow lives here, so it can be changed and run again
while the game stays open. The game only carries out small commands from its input file (TOTK_INPUT_FILE):
  key CODE MS                    a key held MS ms (Mac key codes, input.c button(): the Switch buttons below)
  noclip goto X Y Z / noclip off a noclip flight / back on the ground
  survey pos TAG                 Link's position, answered in the log as "[survey] tag=TAG status=position ... xyz=X,Y,Z"
  autoshrine info N              shrine N's places ("[as-info] N name=... travel=X,Y,Z ...")
  autoshrine face X Y Z          Link turned toward a point
  autoshrine walk X Y Z MS       Link walks toward a point for MS ms
  autoshrine watch on            map and event files the game opens ("[as-file] PATH")
  shot PATH                      a screenshot
The game runs with TOTK_SURVEY=1, TOTK_INPUT_FILE and TOTK_LOG_DIR (and TOTK_AUTOSHRINE_SHOTS is not needed here).

Buttons (Switch names; the Xbox labels on screen are by position): A (right; Xbox B) activates a prompt; B (bottom; Xbox A)
closes a message or a menu; X (top; Xbox Y) skips a cutscene ("Skip X") and jumps in play; + is never pressed.

  python3 scripts/autoshrine.py --run <run folder> [--shrine N] [--count 3]
<run folder> holds input.txt, logs/totk.log and profile/ (the save the game plays). Without --shrine the nearest shrine the
save has not cleared is done next, again and again (--count). Dying is fine: Continue, and the next shrine.
"""
import argparse, glob, math, os, re, struct, sys, time

KEY = {'A': 38, 'B': 40, 'X': 34, 'ZL': 58, 'W': 13, 'S': 1, 'A_LEFT': 0, 'D': 2}
SKIP_KINDS = ('fight', 'Proving Grounds')


def dist(a, b): return math.dist(a, b)
def xz(a, b): return math.hypot(a[0] - b[0], a[2] - b[2])


class Game:
    def __init__(self, run):
        self.input = os.path.join(run, 'input.txt'); self.logpath = os.path.join(run, 'logs', 'totk.log')
        self.profile = os.path.join(run, 'profile'); self.shots = os.path.join(run, 'shots'); os.makedirs(self.shots, exist_ok=True)
        self.off = 0; self.tag = 0; self.files = []; self.nshot = 0
        self.lines()   # the whole log once: the map files opened so far (which shrine Link is in); waits start after it

    def send(self, line):
        with open(self.input, 'a') as f: f.write(line + '\n')

    def lines(self):   # new log lines since the last read; file opens are kept in self.files
        with open(self.logpath, 'rb') as f:
            f.seek(self.off); data = f.read(); self.off += len(data)
        out = data.decode('utf-8', 'replace').splitlines()
        for l in out:
            if l.startswith('[as-file] '): self.files.append((time.time(), l[10:]))
        return out

    def wait(self, pattern, timeout):
        rx = re.compile(pattern); end = time.time() + timeout
        while time.time() < end:
            for l in self.lines():
                m = rx.search(l)
                if m: return m
            time.sleep(0.05)
        return None

    def pos(self, timeout=3):
        self.tag += 1; t = 'p%d' % self.tag; self.send('survey pos ' + t)
        m = self.wait(r'tag=%s status=(\w+).*?xyz=([-\d.]+),([-\d.]+),([-\d.]+)' % t, timeout)
        if not m or m.group(1) != 'position': return None
        return tuple(float(m.group(i)) for i in (2, 3, 4))

    def state(self, timeout=3):   # "ctl state": event, map, stable, hp, prompt, pos, face, cam (runtime/world_survey.c)
        self.send('ctl state')
        m = self.wait(r'\[ctl-state\] (.*)', timeout)
        if not m: return None
        d = dict(kv.split('=', 1) for kv in m.group(1).split() if '=' in kv)
        for k in ('pos', 'cam', 'face', 'camright'):
            try: d[k] = tuple(float(v) for v in d[k].split(','))
            except Exception: d[k] = None
        d['event'] = d.get('event') == '1'
        return d

    def key(self, name, ms=150): self.send('key %d %d' % (KEY[name], ms)); time.sleep(ms / 1000 + 0.05)

    def press(self, name='A', ms=2000):   # a prompt: nothing else held, a quiet moment, then the button HELD (a tap does
        time.sleep(0.5); self.key(name, ms)   # nothing at the shrine's door, 2026-10-28; holding 2 s activated it)

    def shot(self, what):   # the game frame only (the shot is a 3840x2160 canvas, the frame its top-left 1920x1080), 960 wide
        self.nshot += 1; path = '%s/%s-%02d-%s.png' % (self.shots, time.strftime('%H%M%S'), self.nshot, what)
        self.send('shot ' + path)
        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) > 0: break
            time.sleep(0.1)
        time.sleep(0.2)
        try:
            from PIL import Image
            im = Image.open(path); w, h = im.size
            im.crop((0, 0, w // 2, h // 2)).resize((960, 540)).save(path)
        except Exception as e: say('  shot not cropped: %s' % e)
        return path

    def file_since(self, t0, prefix):
        self.lines(); return any(t >= t0 and p.startswith(prefix) for t, p in self.files)


def say(*a): print(time.strftime('%H:%M:%S'), *a, flush=True)


def info(g, n):
    g.send('autoshrine info %d' % n)
    m = g.wait(r'\[as-info\] %d (.*)' % n, 10)
    if not m or m.group(1) == 'none': return None
    d = {'n': n}
    rest = m.group(1)
    d['name'] = re.search(r'name=(.*?) kind=', rest).group(1)
    d['kind'] = re.search(r'kind=(.*?) chests=', rest).group(1)
    for f in ('cave', 'hidden', 'crystal', 'island'):
        m2 = re.search(r'\b%s=(\d)' % f, rest); d[f] = int(m2.group(1)) if m2 else 1   # unknown: treated as not doable
    for k, x, y, z in re.findall(r'(\w+)=([-\d.]+),([-\d.]+),([-\d.]+)', rest): d[k] = (float(x), float(y), float(z))
    return d


def mm3(s):   # MurmurHash3 x86_32, seed 0: the GameData name hash
    d = s.encode(); h = 0; c1, c2 = 0xcc9e2d51, 0x1b873593; n = len(d)
    for i in range(0, n // 4 * 4, 4):
        k = struct.unpack_from('<I', d, i)[0]; k = (k * c1) & 0xffffffff; k = ((k << 15) | (k >> 17)) & 0xffffffff; k = (k * c2) & 0xffffffff
        h ^= k; h = ((h << 13) | (h >> 19)) & 0xffffffff; h = (h * 5 + 0xe6546b64) & 0xffffffff
    k = 0
    for i, b in enumerate(d[n // 4 * 4:]): k |= b << (8 * i)
    if n % 4: k = (k * c1) & 0xffffffff; k = ((k << 15) | (k >> 17)) & 0xffffffff; k = (k * c2) & 0xffffffff; h ^= k
    h ^= n; h ^= h >> 16; h = (h * 0x85ebca6b) & 0xffffffff; h ^= h >> 13; h = (h * 0xc2b2ae35) & 0xffffffff; h ^= h >> 16
    return h


def save_states(profile):   # DungeonState of each shrine in the newest save
    saves = glob.glob(os.path.join(profile, 'save', '*', 'slot_*', 'progress.sav'))
    if not saves: return {}
    b = open(max(saves, key=os.path.getmtime), 'rb').read(); tab = {}; i = 0x28
    while i + 8 <= len(b):
        h, v = struct.unpack_from('<II', b, i)
        if h == 0xa3db7114: break
        tab[h] = v; i += 8
    names = {mm3(s): s for s in ('Hidden', 'Appear', 'Open', 'Enter', 'Clear')}
    return {k: names.get(tab.get(mm3('DungeonState.Dungeon%03d' % k)), '?') for k in range(152)}


def held(g, push='S'):   # True when a cutscene (or a menu) holds Link: a short push of the stick does not move him
    a = g.pos()
    g.key(push, 300)
    b = g.pos()
    return a is not None and b is not None and dist(a, b) < 0.3


def skip_scenes(g, until=None, limit=60):
    """While the game says a main event plays (ctl state event=1): X (the scenes' "Skip X"), then B for a message or the
    item's Get screen. Over when event=0 twice running, or until() says so. Returns 'free', 'until' or 'timeout'."""
    free = 0; end = time.time() + limit
    while time.time() < end:
        if until and until(): return 'until'
        st = g.state()
        if st and st['event']:
            free = 0; g.key('X', 100); time.sleep(0.3); g.key('B', 100); time.sleep(0.3)
        else:
            free += 1
            if free >= 2: return 'free'
            time.sleep(0.4)
    return 'timeout'


def walk_to(g, to, near=1.0, limit=8.0):
    end = time.time() + limit
    while time.time() < end:
        p = g.pos()
        if p and xz(p, to) < near: return p
        g.send('autoshrine walk %.2f %.2f %.2f 500' % to); g.wait(r'\[as-done\] walk', 2)
    return None


def front(s, out=1.6):   # in front of the shrine's door, on the travel point's side
    e, t = s['entrance'], s['travel']; dx, dz = t[0] - e[0], t[2] - e[2]; l = math.hypot(dx, dz) or 1
    return (e[0] + dx / l * out, e[1] + 1.0, e[2] + dz / l * out)


def place(g, to, face):   # noclip straight onto a spot, turned toward face, then down; returns where he stands
    g.send('noclip goto %.2f %.2f %.2f' % (to[0], to[1] + 0.5, to[2])); g.wait(r'\[noclip\] arrived', 30); time.sleep(0.8)
    g.send('autoshrine face %.2f %.2f %.2f' % face); g.wait(r'\[as-done\] face', 3)
    g.send('noclip off'); time.sleep(1.2)
    return g.pos()


def toward(a, b, d):   # the point d metres from a toward b (level)
    dx, dz = b[0] - a[0], b[2] - a[2]; l = math.hypot(dx, dz) or 1
    return (a[0] + dx / l * d, a[1], a[2] + dz / l * d)


def turn_to(g, target, tol=15.0, tries=12):
    """Link turned to face target: short taps of the left stick toward it, worked out from the camera (the stick is camera
    relative); checked against his facing from ctl state. Returns the angle left, in degrees."""
    ang = 180.0
    for _ in range(tries):
        st = g.state()
        if not st or not st['pos'] or not st['face'] or not st['cam'] or not st['camright']: time.sleep(0.2); continue
        p = st['pos']; dx, dz = target[0] - p[0], target[2] - p[2]; l = math.hypot(dx, dz) or 1; dx /= l; dz /= l
        fx, fz = st['face']; ang = math.degrees(math.acos(max(-1, min(1, fx * dx + fz * dz))))
        if ang <= tol: return ang
        cfx, cfz = p[0] - st['cam'][0], p[2] - st['cam'][2]; cl = math.hypot(cfx, cfz) or 1; cfx /= cl; cfz /= cl
        rx, rz = st['camright']
        sx, sy = dx * rx + dz * rz, dx * cfx + dz * cfz; sl = math.hypot(sx, sy) or 1
        g.send('ctl stick l %.3f %.3f 90' % (sx / sl, sy / sl)); time.sleep(0.25)
    return ang


def approach(g, target, back_from, near, start_back=4.0):
    """Link set down start_back metres out from target (back_from's side), walked up to it, then turned to face it (checked
    with ctl state) and the camera put behind him (ZL)."""
    place(g, toward(target, back_from, start_back), target)
    end = time.time() + 8
    while time.time() < end:
        p = g.pos()
        if p and xz(p, target) <= near: break
        g.send('autoshrine walk %.2f %.2f %.2f 300' % target); g.wait(r'\[as-done\] walk', 2)
    left = turn_to(g, target); g.key('ZL', 150); time.sleep(0.4)
    say('  at %.1f m, facing it to %.0f degrees' % (xz(g.pos() or target, target), left))
    return g.pos()


def hold_prompt(g, target, back_from, near, reacted_m=1.0, tries=2, what='prompt'):
    """A prompt that wants A HELD, reached facing it (approach); reacted when the scene moves Link reacted_m or more."""
    for i in range(tries):
        p0 = approach(g, target, back_from, near); time.sleep(1.0)
        st = g.state(); g.shot('%s-%d' % (what, i))
        say('  prompt: %s' % (st and st.get('prompt')))
        g.press('A', 2000); say('  A held at the %s (try %d)' % (what, i + 1))
        end = time.time() + 3
        while time.time() < end:
            p = g.pos()
            if p and p0 and dist(p, p0) >= reacted_m: return True
            time.sleep(0.2)
    return False


def fly(g, to, hold=2.0):
    g.send('noclip goto %.2f %.2f %.2f' % to)
    if not g.wait(r'\[noclip\] arrived', 120): return False
    time.sleep(hold); g.send('noclip off'); time.sleep(2.0)
    p = g.pos()
    if p is not None and p[1] < to[1] - 10:   # falling (a chasm under the spot): caught with noclip, back up
        g.send('noclip goto %.2f %.2f %.2f' % (to[0], to[1] + 3, to[2])); g.wait(r'\[noclip\] arrived', 60)
        say('  fell after landing: caught, the shrine is left'); return False
    return p is not None and xz(p, to) < 4


def prompt(g, look, tries=3, reacted=None, wait=2.0):
    """A at a prompt: face it, a clean A, watch reacted() for wait seconds; again up to tries times."""
    for i in range(tries):
        g.send('autoshrine face %.2f %.2f %.2f' % look); g.wait(r'\[as-done\] face', 2)
        t0 = time.time(); g.press('A'); say('  A (try %d)' % (i + 1))
        while time.time() < t0 + wait:
            if reacted(t0): return True
            time.sleep(0.2)
    return False


def game_over_check(g):   # Link not moving for B and the screen not changing: A on the Game Over screen's Continue
    g.key('B', 100); time.sleep(0.5)
    if not held(g): return False
    g.press('A'); time.sleep(8)
    return True


def do_shrine(g, s, state):
    n = s['n']; say('shrine %d %s (%s), save says %s' % (n, s['name'], s['kind'], state))
    # 1. go to location
    if not fly(g, s['travel']): g.shot('fly-failed'); return 'fly failed (returned or not landed)'
    g.shot('landed')
    # 2. activate location
    if state in ('Appear', '?'):
        # 1.6 m out from the door showed "Examine" (2026-10-28); 1.2 and 2.0 m if not
        ok = hold_prompt(g, s['entrance'], s['travel'], near=1.6, what='door')   # 1.6 m from the door showed "Examine"
        g.shot('gate')
        if ok: skip_scenes(g, limit=40)
        else: say('  the door did not react (already open?); walking in anyway')
    # 3. enter location: the doorway and the lift take A HELD (Mayamats went in that way); judged by position, 6 s each
    inside = lambda: (lambda st: bool(st) and 'Dungeon%03d' % n in st.get('map', ''))(g.state())   # the game's own map name
    def wait_inside(sec=6):
        end = time.time() + sec
        while time.time() < end:
            if inside(): return True
            time.sleep(0.3)
        return False
    if not inside():   # walked in through the doorway onto the lift (facing it), A held there
        hold_prompt(g, s['lift_floor'], s['entrance'], near=0.8, reacted_m=99, tries=1, what='lift')
        wait_inside(6)
    if not inside():
        end = time.time() + 30   # the lift ride and the load
        while time.time() < end and not inside(): time.sleep(0.5)
    if not inside(): g.shot('not-in'); return 'enter: not inside after A held at the door and the lift'
    return do_inside(g, s)


def do_inside(g, s):   # from inside the shrine: the altar, its scene, out
    say('  inside %s' % s['name']); g.shot('inside')
    skip_scenes(g, limit=30)
    # 4. go to location exit, 5. exit location: on the altar's floor point, A held; its scene moves Link (1.3 m at Mayamats)
    if not hold_prompt(g, s['goal_floor'], s['start'], near=0.8, reacted_m=0.8, what='altar'):
        g.shot('altar-no-scene'); return 'altar: A got no scene'
    say('  the blessing started')
    # 6. the exit: until Link is back on the surface. A scene holding him: X (its skip) and B (messages, the item's Get).
    # Free but still inside (the scene is over): A held at the altar again answers "Do you want to leave?" (2026-10-28,
    # Turakawak). The start lift is not used: its spot dropped him through the floor.
    outside = lambda: (lambda st: bool(st) and st.get('map', '').startswith('MainField'))(g.state())   # the game's map name
    end = time.time() + 90; leaves = 0
    while time.time() < end and not outside():
        if held(g):
            g.key('X', 100); time.sleep(0.25); g.key('X', 100); g.key('B', 100)
        elif leaves < 3:
            leaves += 1; say('  free inside: A held at the altar to leave (%d)' % leaves)
            hold_prompt(g, s['goal_floor'], s['start'], near=0.8, reacted_m=0.8, tries=1, what='altar')
    if not outside(): g.shot('not-out'); return 'could not get out'
    say('  outside'); g.shot('outside')
    skip_scenes(g, limit=20)
    return 'done'


def inside_shrine(g, shrines, p):   # the shrine whose map the game loaded last, when Link is in shrine coordinates (all the
    if p is None or p[1] < -100 or abs(p[0]) > 1000 or abs(p[2]) > 1000: return None   # interiors share them: the map file says which)
    g.lines(); last = None
    for t, f in g.files:
        m = re.match(r'Banc/SmallDungeon/Dungeon(\d+)_', f)
        if m: last = int(m.group(1))
        elif f.startswith('Banc/MainField/'): last = None
    return next((s for s in shrines if s['n'] == last), None)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--run', required=True); ap.add_argument('--shrine', type=int); ap.add_argument('--count', type=int, default=3)
    a = ap.parse_args()
    g = Game(a.run); g.send('autoshrine watch on'); g.wait(r'\[as-done\] watch', 10)
    need = ('travel', 'entrance', 'lift', 'lift_floor', 'start', 'goal', 'goal_floor', 'back', 'back_floor')
    shrines = []
    for n in range(152):   # a line can come back cut (other log output in between): ask again
        for _ in range(3):
            s = info(g, n)
            if s and all(k in s for k in need): shrines.append(s); break
    say('read %d shrines' % len(shrines))
    tried = set(); results = []
    for _ in range(a.count if a.shrine is None else 1):
        states = save_states(g.profile); here = g.pos() or (0, 0, 0)
        st0 = g.state() or {}; m0 = re.search(r'Dungeon(\d+)', st0.get('map', ''))   # where Link is, from the game's map name
        cur = next((s for s in shrines if m0 and s['n'] == int(m0.group(1))), None)   # where is Link before any move: inside a shrine, finish that one first
        if cur:
            say('Link is inside %s: finishing it' % cur['name']); tried.add(cur['n'])
            r = do_inside(g, cur); say('  -> %s' % r); results.append((cur['name'], r)); time.sleep(3); continue
        if a.shrine is not None: pick = [s for s in shrines if s['n'] == a.shrine]
        else:
            pick = sorted((s for s in shrines if s['n'] not in tried and s['kind'] not in SKIP_KINDS and not (s['cave'] or s['hidden'] or s['crystal']) and s['travel'][1] > -100
                           and states.get(s['n']) in ('Appear', 'Open', 'Enter')),   # caves: the flight would end in rock or a chasm
                          key=lambda s: xz(here, s['travel']))
        if not pick: say('nothing left'); break
        s = pick[0]; tried.add(s['n'])
        r = do_shrine(g, s, states.get(s['n'], '?'))
        say('  -> %s' % r); results.append((s['name'], r))
        time.sleep(3)
    say('summary:', '; '.join('%s: %s' % x for x in results))


if __name__ == '__main__':
    main()
