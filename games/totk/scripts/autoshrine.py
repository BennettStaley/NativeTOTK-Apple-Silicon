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


def toward(a, b, d):   # the point d metres from a toward b (level)
    dx, dz = b[0] - a[0], b[2] - a[2]; l = math.hypot(dx, dz) or 1
    return (a[0] + dx / l * d, a[1], a[2] + dz / l * d)


def cam_yaw(st):   # where the camera looks (level), in degrees
    p, c = st['pos'], st['cam']; return math.degrees(math.atan2(p[2] - c[2], p[0] - c[0]))


def offsets(g, target):   # (camera off, Link off) from target in degrees, and the state
    st = g.state()
    if not st or not st['pos'] or not st['cam'] or not st['face']: return None, None, st
    p = st['pos']; want = math.degrees(math.atan2(target[2] - p[2], target[0] - p[0]))
    fx, fz = st['face']
    return (want - cam_yaw(st) + 540) % 360 - 180, (want - math.degrees(math.atan2(fz, fx)) + 540) % 360 - 180, st


def camera_on(g, target, tol=8.0, tries=10):   # right stick: about 86 degrees a second at full push (2026-10-28)
    for _ in range(tries):
        cam, _, st = offsets(g, target)
        if cam is None: time.sleep(0.2); continue
        if abs(cam) <= tol: return True
        ms = max(60, min(500, abs(cam) / 86.0 * 1000))
        g.send('ctl stick r %d 0 %d' % (1 if cam > 0 else -1, ms)); time.sleep(ms / 1000 + 0.25)
    return False


def face(g, target, tol=15.0, tries=8):   # camera on it, then short taps of the left stick straight up turn Link to it
    for _ in range(tries):
        camera_on(g, target)
        _, link, st = offsets(g, target)
        if link is None: time.sleep(0.2); continue
        if abs(link) <= tol: return True
        g.send('ctl stick l 0 1 50'); time.sleep(0.35)
    return False


def walk_to_spot(g, spot, near=0.5, limit=15, stop=None):
    """Walked toward spot (camera re-aimed each step, left stick straight up) until within near metres; steps shrink to
    ~0.1 m inside 2 m so the prompt's window is not passed; stops as soon as the distance stops shrinking (at or past it,
    or blocked). stop(st) ends it early (an event, a map)."""
    end = time.time() + limit; last = 1e9
    while time.time() < end:
        _, _, st = offsets(g, spot)
        if st and stop and stop(st): return 'stopped'
        if not st or not st['pos']: time.sleep(0.2); continue
        d = xz(st['pos'], spot)
        if d <= near: return 'there'
        if d > last - 0.02 and d < 2.0: return 'closest'   # no closer: the nearest point on this line
        last = d
        camera_on(g, spot, tol=6.0)
        g.send('ctl stick l 0 1 %d' % (300 if d > 4 else 150 if d > 2 else 40)); time.sleep(0.35)
    return 'timeout'


def prompt_visible(g):
    """An A prompt ("Examine" etc.) on screen: its button circle and word, just below and right of Link, at x 545-625,
    y 423-446 of the 960x540 game frame: 367-450 bright pixels (r+g+b > 600) with it, 0-2 without, a white lantern
    beside it included (checked on 40 shots, 2026-10-28)."""
    from PIL import Image
    path = g.shot('p'); time.sleep(0.05)
    try: im = Image.open(path).convert('RGB').crop((545, 423, 625, 446))
    except Exception: return False
    finally:
        try: os.remove(path)
        except OSError: pass
    return sum(1 for px in im.getdata() if sum(px) > 600) >= 150


def walk_until_prompt(g, target, limit=20):
    """Camera on target, Link stepped toward it in small steps (about 0.15 m) until its prompt shows; stops there (no
    overshoot), then turns him to face it. Returns True with the prompt showing."""
    end = time.time() + limit; last = 1e9
    while time.time() < end:
        if prompt_visible(g): face(g, target); return prompt_visible(g) or True
        camera_on(g, target, tol=6.0)
        st = g.state()
        if st and st['pos']:
            d = xz(st['pos'], target)
            if d > last + 0.5: break   # walking away: past it
            last = min(last, d)
            g.send('ctl stick l 0 1 %d' % (200 if d > 4 else 20)); time.sleep(0.3)
    return False


def prompt_at(g, target, what, tries=2, moved_m=0.8):
    """The verified way (2026-10-28): camera on the target, small steps until its prompt shows, Link facing it, A held 2 s;
    it worked when an event starts (event=1) or the scene moves Link."""
    for i in range(tries):
        ok = walk_until_prompt(g, target)
        say('  %s: prompt %s' % (what, 'showing' if ok else 'not found'))
        if not ok: g.shot('%s-noprompt-%d' % (what, i)); g.send('ctl stick l 0 -1 400'); time.sleep(0.6); continue
        p0 = g.pos(); g.press('A', 2000)
        end = time.time() + 3
        while time.time() < end:
            st = g.state()
            if st and (st['event'] or (p0 and dist(st['pos'], p0) >= moved_m)): return True
            time.sleep(0.3)
    return False


def skip_scenes(g, until=None, limit=90):
    """While a main event plays (event=1): X held 1 s (its "Skip X"), then B (messages, the item's Get screen).
    Over when event=0 twice running, or until() says so."""
    free = 0; end = time.time() + limit
    while time.time() < end:
        st = g.state()
        if until and st and until(st): return 'until'
        if st and st['event']:
            free = 0; g.key('X', 1000); time.sleep(0.2); g.key('B', 100); time.sleep(0.2)
        else:
            free += 1
            if free >= 2: return 'free'
            time.sleep(0.4)
    return 'timeout'


def place(g, to, look):   # noclip straight onto a spot (never at or inside a door), then down
    g.send('noclip goto %.2f %.2f %.2f' % (to[0], to[1] + 0.5, to[2])); g.wait(r'\[noclip\] arrived', 30); time.sleep(0.8)
    g.send('noclip off'); time.sleep(1.2)
    return g.pos()


def fly(g, to, hold=2.0):
    g.send('noclip goto %.2f %.2f %.2f' % to)
    if not g.wait(r'\[noclip\] arrived', 120): return False
    time.sleep(hold); g.send('noclip off'); time.sleep(2.0)
    p = g.pos()
    if p is not None and p[1] < to[1] - 10:   # falling (a chasm under the spot): caught with noclip, back up
        g.send('noclip goto %.2f %.2f %.2f' % (to[0], to[1] + 3, to[2])); g.wait(r'\[noclip\] arrived', 60)
        say('  fell after landing: caught, the shrine is left'); return False
    return p is not None and xz(p, to) < 4


def do_shrine(g, s, state):
    n = s['n']; say('shrine %d %s (%s), save says %s' % (n, s['name'], s['kind'], state))
    in_map = lambda st: 'Dungeon%03d' % n in st.get('map', '')
    # 1. go to location: the travel point (outside, in front of the door)
    if not fly(g, s['travel']): g.shot('fly-failed'); return 'fly failed (returned, fell or not landed)'
    # 2. activate location
    if state in ('Appear', '?'):
        if prompt_at(g, s['entrance'], 'door'): skip_scenes(g, limit=30)
        else: say('  the door gave no scene (already open?)')
    # 3. enter location: walk through the door onto the lift; its scene starts on its own, then the shrine's map
    r = walk_to_spot(g, s['lift_floor'], near=0.6, stop=lambda st: st['event'] or in_map(st))
    if r not in ('stopped',):
        prompt_at(g, s['lift'], 'lift', tries=1)
    end = time.time() + 40
    while time.time() < end:
        st = g.state()
        if st and in_map(st) and not st['event']: break
        if st and st['event']: g.key('X', 1000)
        time.sleep(0.5)
    else: g.shot('not-in'); return 'enter: the shrine did not load'
    return do_inside(g, s)


def do_inside(g, s):   # inside: the altar (its floor point, facing it), its scene skipped, out
    say('  inside %s' % s['name'])
    skip_scenes(g, limit=20)
    place(g, toward(s['goal_floor'], s['start'], 4.0), s['goal'])   # 4 m short of the altar's floor point
    if not prompt_at(g, s['goal'], 'altar'): g.shot('altar-no-scene'); return 'altar: A got no scene'
    say('  the blessing started')
    outside = lambda st: st.get('map', '').startswith('MainField')
    r = skip_scenes(g, until=outside, limit=90)
    if r == 'free':   # the scene is over and he is still inside: the altar asks "Do you want to leave?"
        prompt_at(g, s['goal'], 'leave', tries=1)
        r = skip_scenes(g, until=outside, limit=60)
    if r != 'until': g.shot('not-out'); return 'could not get out'
    say('  outside'); skip_scenes(g, limit=15)
    return 'done'


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
