# Rewrite structural check: no code is run, the rewriter's build output is checked word by word. Denominators are recounted with an independent disassembler (llvm-objdump), not relying on the rewriter's own capstone scan.
# Usage: structck.py <rewrite build output dir> <module> <NSO>     last output line STRUCT <module> OK|FAIL ..., exit code 0/1
# Checks:
#  ⓪ Registered cuts (cuts.tsv): build ID matches, original image at that site == original bytes, rewritten image == new bytes, reason non-empty, no overlap with rewrite sites / windows; host-call gates introduced by cuts (svc #N≥0x80) have number, offset and form checked against the hle.h registry row (entry form svc;ret at a function entry / call-site form where the original instruction is bl and the target matches the registration)
#  ① Non-text segments are byte-for-byte identical to the original image except for cuts; everything from the end of the image to the patch area is 0
#  ② Changed words in text == rewrite site heads (non-J rows of sites.tsv + window starts) ∪ cuts inside text; all rewrite site heads are a B into the patch area
#  ③ Independent denominator: llvm-objdump reads the original text; x18/w18 operands, tpidr_el0 reads/writes, tpidrro_el0 reads, svc, cntpct/cntvct
#     all fall within rewrite sites or windows; conversely, every rewrite site is in the independent set (except window starts)
#  ④ The patch area no longer contains x18/w18, tpidr_el0, svc, cntpct/cntvct
#  ⑤ Host gate (svc / timer) blocks equal the template word for word, and match gates.tsv one to one (return point, kind, svc number / destination register, original address)
#  ⑧ Parameter gates (params.tsv): the two original words build a constant with 64-bit movz/movk, the two new words = mrs tpidrro / ldr key slot, the register is not touched in between, no branch target in the range, key number registered, a unique original value per key
#  ⑦ Registered observation gates (obs.tsv): original image at that site == the registered original instruction word; the original instruction is relocatable (not PC-relative / not a branch / does not involve x18 / does not read or write system registers, judged by capstone);
#     may be in the middle of a function (the gate ABI saves and restores everything, checked by gatefn); no overlap with rewrite sites / windows / cuts; the gate block = the template word for word (the same first half as an svc gate + original instruction + b back to off+4), gates.tsv records an obs row
#  ⑩ Trampoline exits: for every b jumping into the patch area, the patch block's exits back to text may only be original site+4 / end of the absorbed span / end of the window / a branch target of an original instruction within the span (registered cuts are let through and listed)
#  ⑪ Branch gates introduced by cuts (hle.h registry "branch gate" rows): block = template word for word, x8 restored on both paths, matches gates.tsv
#  ⑥ Thread slots: the four key numbers in the header are mutually distinct and < 512; every mrs xN, tpidrro_el0 in the patch area is followed (skipping adrp/add that do not write xN) by ldr/str xM,[xN,#key number×8]
import sys, os, re, struct, subprocess, tempfile
sys.path.insert(0, os.path.dirname(__file__))
import nsoimg
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # Workspace root (game files, build outputs and run artifacts are all under it)

LLVM = os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm@23/bin')
out_dir, mod, nso = sys.argv[1:4]
orig = nsoimg.load(nso)
h = nsoimg.header(out_dir, mod)
PO, TE = h['PATCH_OFF'], h['TEXT_END']
pat = open(f'{out_dir}/{mod}.patched.bin', 'rb').read()
W = lambda b, o: struct.unpack_from('<I', b, o)[0]
fails = []
def bad(msg):
    fails.append(msg)
    if len(fails) <= 30: print('FAIL', msg)

def llvm_dis(blob, base):
    """Disassemble from raw bytes and return the relevant lines {offset: lowercase instruction text}."""
    with tempfile.TemporaryDirectory() as td:
        b, e = os.path.join(td, 'x.bin'), os.path.join(td, 'x.elf')
        open(b, 'wb').write(blob)
        subprocess.run([f'{LLVM}/llvm-objcopy', '-I', 'binary', '-O', 'elf64-littleaarch64',
                        '--rename-section', '.data=.text,code', b, e], check=True)
        # Keep only potentially relevant lines (x18/w18, tpidr, svc, cnt?ct); filtering millions of lines is left to grep
        od = subprocess.Popen([f'{LLVM}/llvm-objdump', '-d', '--no-show-raw-insn', e], stdout=subprocess.PIPE)
        r = subprocess.run(['/usr/bin/grep', '-iE', r'[xw]18|tpidr|svc|cnt[pv]ct'], stdin=od.stdout, capture_output=True, text=True)
        od.stdout.close(); assert od.wait() == 0
    out = {}
    for line in r.stdout.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)$', line)
        if m: out[base + int(m.group(1), 16)] = m.group(2).split('//')[0].split('<')[0].strip().lower()
    return out

RX18 = re.compile(r'\b[xw]18\b')
def sysk(t):
    if re.match(r'svc\s', t): return 'svc'
    if re.match(r'mrs\s', t) and 'tpidr_el0' in t: return 'tpidr_r'
    if re.match(r'msr\s', t) and 'tpidr_el0' in t: return 'tpidr_w'
    if re.match(r'mrs\s', t) and 'tpidrro_el0' in t: return 'tpidrro_r'
    if re.match(r'mrs\s', t) and 'cntpct_el0' in t: return 'cntpct'
    if re.match(r'mrs\s', t) and 'cntvct_el0' in t: return 'cntvct'
    return None

# ---- sites.tsv ----
sites = {}; wins = []; jumps = 0; calls = {}; spins = {}
for l in open(f'{out_dir}/{mod}.sites.tsv'):
    f = l.split(); a = int(f[0], 16)
    if f[1] == 'J': jumps += 1; continue
    if f[1] == 'S': spins[a] = [int(x, 16) for x in f[2:]]; continue   # spin wait (patch_nso.py "Spin waits"): head, back-edges; checked in ⑫
    if f[1] == 'W': wins.append((a, int(f[2], 16))); sites[a] = 'W'
    elif f[1] == 'C':
        end = int(f[2], 16)
        if end != a + 8: bad(f'call window must have two words {a:#x}')
        calls[a] = end - 4; wins.append((a, end)); sites[a] = 'C'
    else: sites[a] = f[1]
inwin = set(o for lo, hi in wins for o in range(lo, hi, 4))
atomic_groups = nsoimg.exclusive_groups(orig, [(lo, hi) for lo, hi in wins if lo not in calls])

# ⓪ Registered cuts
cuts = []; bid = None
for l in open(f'{out_dir}/{mod}.cuts.tsv'):
    if l.startswith('# BUILD_ID'): bid = l.split()[-1]; continue
    f = l.rstrip('\n').split('\t')
    if len(f) == 4: f = f[:3] + ['all'] + f[3:]                    # Old 4-column format: scope defaults to all
    scope_alias = {'\u5168': 'all', '\u4ec5\u5bbf\u4e3b': 'host-only'}   # scope values written by older patch_nso.py builds (pre-English cuts.tsv in existing caches)
    f[3] = scope_alias.get(f[3], f[3])
    off, ob, nb, scope, why = f[:5]
    if scope not in ('all', 'host-only'): bad(f'cut {off} has unknown scope: {scope}')
    cuts.append((int(off, 16), bytes.fromhex(ob), bytes.fromhex(nb), why))
if bid != open(nso, 'rb').read(0x50)[0x40:0x50].hex(): bad(f'cuts.tsv build id {bid} does not match the NSO')
cutb = set()
for off, ob, nb, why in cuts:
    if orig[off:off + len(ob)] != ob: bad(f'cut {off:#x} original bytes do not match')
    for k in range(0, len(nb), 4):                               # The svc words in the new bytes are subsequently rewritten into gates (head is B) and checked as rewrite sites; skipped here
        if int.from_bytes(nb[k:k + 4], 'little') & 0xFFE0001F == 0xD4000001: continue
        if pat[off + k:off + k + len(nb[k:k + 4])] != nb[k:k + 4]: bad(f'cut {off:#x}+{k} rewritten image is not the new bytes'); break
    if not why.strip() or len(ob) != len(nb): bad(f'cut {off:#x} has an empty reason or unequal lengths')
    cutb |= set(range(off, off + len(ob)))
# Host-call gates produced by cuts: the word in the cut's new bytes that decodes to svc #N (N ≥ 0x80, must be in the hle.h host-call number registry SVC_HOST_*)
_hostcall = {}
registry_path = os.environ.get('DM_HOST_REGISTRY', os.path.join(os.path.dirname(__file__), '..', 'host', 'hle.h'))
registry_text = open(registry_path).read() if os.path.isfile(registry_path) else ''
_reg = {int(v, 16) for v in re.findall(r'SVC_HOST_\w+\s*=\s*(0x[0-9a-fA-F]+)', registry_text)}
for off, ob, nb, why in cuts:
    for k in range(0, len(nb), 4):
        w = int.from_bytes(nb[k:k + 4], 'little')
        if w & 0xFFE0001F == 0xD4000001:
            n = (w >> 5) & 0xFFFF; a = off + k
            if n < 0x80: bad(f'cut {off:#x} introduces svc #{n:#x}: numbers below 0x80 are the Horizon range, cuts must not use them')
            elif n not in _reg: bad(f'cut {off:#x} introduces svc #{n:#x}: not in the hle.h host-call registry')
            elif sites.get(a) != 'svc': bad(f'svc #{n:#x} introduced by cut {off:#x} was not rewritten into a host gate (sites.tsv has no {a:#x} svc)')
            _hostcall[a] = n
# The form of a host-call gate is checked against its row in the hle.h registry (the "number ... cut (main offset)" column):
#   entry form ("... entry"): new bytes = svc #N ; ret, the offset is an eh_frame function entry - the gate saves and restores x30, ret returns to the original caller;
#   call-site form ("bl 0x<target> ..."): the original instruction must be bl with target = the registered target, new bytes = a single svc #N, the gate returns to off+4 (checked by the final b of the ⑤ template).
#   Register rationale for the call-site form: at a bl, per AAPCS the callee could clobber x9..x17, x16/x17, x30 and NZCV anyway, so the gate's side effects on these do not change caller-visible behavior;
#   the x0..x7 return values and x19..x28 / sp are preserved by the host gate contract (checked gate by gate by gatefn).
_hreg = {}
for l in registry_text.splitlines():
    # registry row: // 0x<number> ... 0x<main offset> (<form>); older registries used full-width parentheses
    m = re.match(r'//\s+(0x[0-9a-fA-F]+)\s.*?\s(0x[0-9a-fA-F]+)\uff08([^\uff09]*)', l) or re.match(r'//\s+(0x[0-9a-fA-F]+)\s.*?\s(0x[0-9a-fA-F]+) \(([^)]*)', l)
    if m: _hreg[int(m[1], 16)] = (int(m[2], 16), m[3])
def _bl(w, o): return o + 4 * (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) if w & 0xFC000000 == 0x94000000 else None
for a, n in _hostcall.items():
    if n not in _hreg: bad(f'host call svc #{n:#x}: no matching row in the hle.h registry'); continue
    roff, form = _hreg[n]
    if mod != 'main' or roff != a: bad(f'host call svc #{n:#x} is at {mod} {a:#x}, the registry says main {roff:#x}'); continue
    nbw = next(c[2] for c in cuts if c[0] <= a < c[0] + len(c[2]))
    mt = re.search(r'\bbl (0x[0-9a-fA-F]+)', form)
    if mt:
        t = _bl(W(orig, a), a)
        if t is None: bad(f'host call svc #{n:#x} {a:#x}: registered as call-site form, original instruction {W(orig, a):#010x} is not bl')
        elif t != int(mt[1], 16): bad(f'host call svc #{n:#x} {a:#x}: original bl target {t:#x} != registered {mt[1]}')
        if len(nbw) != 4: bad(f'host call svc #{n:#x} {a:#x}: a call-site form cut may replace only this one instruction')
    elif re.search(r'\bentry\b|\u5165\u53e3', form):   # older registries wrote the entry marker in Chinese
        if a not in set(nsoimg.func_starts(orig, TE)): bad(f'host call svc #{n:#x} {a:#x}: registered as entry form, but not an eh_frame function entry')
        if nbw[4:8] != (0xD65F03C0).to_bytes(4, 'little'): bad(f'host call svc #{n:#x} {a:#x}: entry form new bytes are not svc ; ret')
    else: bad(f'host call svc #{n:#x}: unrecognized registry form (must contain a bl 0x<target> or entry marker): {form}')
if cutb & {a + i for a in list(sites) + list(inwin) if a not in _hostcall for i in range(4)}: bad('cut overlaps a rewrite site / window')
ncut_code = sum(1 for c in cuts if c[0] < TE); ncut_data = len(cuts) - ncut_code
obs = []
obs_p = f'{out_dir}/{mod}.obs.tsv'
if os.path.exists(obs_p):
    for l in open(obs_p, encoding='utf-8'):
        if l.startswith('# BUILD_ID'):
            if l.split()[-1] != open(nso, 'rb').read(0x50)[0x40:0x50].hex(): bad('obs.tsv build id does not match the NSO')
            continue
        off, oid, ow, why = l.rstrip('\n').split('\t', 3); obs.append((int(off, 16), int(oid), int(ow, 16), why))
obs_heads = {o for o, *_ in obs}
params = []                                                          # Parameter gates (params.tsv, since r11): (A offset, B offset, register number, key name)
par_p = f'{out_dir}/{mod}.params.tsv'
if os.path.exists(par_p):
    for l in open(par_p, encoding='utf-8'):
        if l.startswith('# BUILD_ID'):
            if l.split()[-1] != open(nso, 'rb').read(0x50)[0x40:0x50].hex(): bad('params.tsv build id does not match the NSO')
            continue
        a, b, r, k, why = l.rstrip('\n').split('\t', 4)
        if not why.strip(): bad(f'parameter gate {a} has an empty reason')
        params.append((int(a, 16), int(b, 16), int(r.lstrip('x')), k))
par_words = {o for a, b, r, k in params for o in (a, b)}

# ① Non-text segments: compare whole blocks first, then byte by byte where blocks differ; differences may only fall inside cuts (cut bytes were already checked against their expected values in ⓪)
nd = []
for blk in range(TE, len(orig), 4096):
    e = min(blk + 4096, len(orig))
    if pat[blk:e] != orig[blk:e]: nd += [o for o in range(blk, e) if pat[o] != orig[o] and o not in cutb]
if nd: bad(f'non-text segments differ from the original image (outside cuts) {len(nd)} bytes e.g. {[hex(x) for x in nd[:5]]}')
if any(pat[len(orig):PO]): bad('nonzero bytes between the image end and the patch area')

# ② text changes == rewrite site heads
diff = set()
for blk in range(0, TE & ~3, 4096):                    # Compare whole blocks first, then word by word where blocks differ
    e = min(blk + 4096, TE & ~3)
    if orig[blk:e] != pat[blk:e]: diff |= {o for o in range(blk, e, 4) if W(orig, o) != W(pat, o)}
heads = set(sites)
cutw = {o & ~3 for o in cutb if o < TE}
spin_bes = {b for bes in spins.values() for b in bes}
_exp = (heads | set(calls.values()) | obs_heads | par_words | spin_bes) - cutw; _got = diff - cutw
if _got != _exp: bad(f'text changes do not match rewrite sites: extra {len(_got - _exp)} missing {len(_exp - _got)} e.g. {[hex(x) for x in sorted(_got ^ _exp)[:5]]}')
def btarget(o):
    w = W(pat, o)
    if w & 0xFC000000 != 0x14000000: return None
    imm = w & 0x3FFFFFF
    if imm & 0x2000000: imm -= 0x4000000
    return o + 4 * imm
nb = [a for a in heads | obs_heads | spin_bes if not (btarget(a) is not None and PO <= btarget(a) < len(pat))]
if nb: bad(f'rewrite site heads that are not a B into the patch area: {len(nb)} e.g. {[hex(x) for x in nb[:5]]}')

# ③ Independent denominator
def cached_dis(blob):
    """The original image is fixed data: relevant lines are cached by sha (a new llvm version gets a new key), so the ten-odd seconds of disassembly are paid only the first time."""
    import hashlib, json
    cd = os.environ.get('DM_VCACHE', os.path.join(DM_ROOT, 'build/cache/dm/verify'))
    ver = os.path.basename(os.path.dirname(LLVM.rstrip('/')))
    p = os.path.join(cd, f'{mod}.{hashlib.sha256(blob).hexdigest()[:16]}.{ver}.json')
    if os.path.exists(p): return {int(k): v for k, v in json.load(open(p)).items()}
    d = llvm_dis(blob, 0)
    os.makedirs(cd, exist_ok=True); tmp = f'{p}.new.{os.getpid()}'; json.dump(d, open(tmp, 'w')); os.replace(tmp, p)
    return d
D = cached_dis(orig[:TE])
ind = {}
for o, t in D.items():
    if RX18.search(t): ind[o] = 'x18'
    k = sysk(t)
    if k: ind[o] = k
miss = sorted(o for o in ind if o not in sites and o not in inwin)
if miss: bad(f'in the independent disassembly but not covered by rewrite sites: {len(miss)} e.g. {[(hex(x), D[x]) for x in miss[:5]]}')
extra = sorted(a for a, k in sites.items() if k not in ('W', 'C') and a not in ind and a not in _hostcall)
if extra: bad(f'rewrite sites not in the independent set: {len(extra)} e.g. {[(hex(x), D.get(x)) for x in extra[:5]]}')
kmis = sorted(a for a, k in sites.items() if k not in ('W', 'C') and a in ind and ind[a] != k)
if kmis: bad(f'rewrite site kind differs from the independent reading: {len(kmis)} e.g. {[(hex(x), sites[x], D[x]) for x in kmis[:5]]}')
nwin_ref = sum(1 for o in ind if o in inwin)

# ④ Patch area is clean
P = llvm_dis(pat[PO:], PO)
left = sorted(o for o, t in P.items() if RX18.search(t) or sysk(t) not in (None, 'tpidrro_r'))
if left: bad(f'patch area still has x18/system register/svc: {len(left)} e.g. {[(hex(x), P[x]) for x in left[:5]]}')

# ⑤ Host gates
KEY = {k[4:-4]: v for k, v in h.items() if k.startswith('KEY_') and k.endswith('_IDX')}
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
_md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); _md.detail = True
def keyseq(at):
    q = list(_md.disasm(pat[at:at + 20], at))
    if len(q) < 5 or q[0].mnemonic != 'mrs' or 'tpidrro_el0' not in q[0].op_str \
            or q[1].mnemonic != 'adrp' or q[1].operands[1].imm != h['KEY_PAGE_OFF'] \
            or q[2].mnemonic != 'ldr' or q[2].operands[1].mem.base != q[1].operands[0].reg \
            or q[3].mnemonic != 'add' or q[3].operands[0].reg != q[0].operands[0].reg \
            or q[3].operands[1].reg != q[0].operands[0].reg:
        return None
    d = q[2].operands[1].mem.disp
    return (q[3].address + 4, d // 8) if d % 8 == 0 and 0 <= d // 8 < 8 else None
# C windows intentionally change the call word as well as the preceding head.
# Inspect the actual decoded sequence, including the original link-producing PC.
for prefix, call in calls.items():
    start = btarget(prefix)
    if start is None or not PO <= start < len(pat):
        bad(f'call preparation has no trampoline {prefix:#x}'); continue
    decoded = list(_md.disasm(pat[start:start + 36], start))
    first = next(_md.disasm(orig[prefix:prefix + 4], prefix), None)
    safe = first and first.mnemonic in ('str', 'stp', 'stur', 'ldr', 'ldp', 'ldur') and not first.writeback \
        and first.reg_name(first.operands[-1].mem.base) in ('sp', 'fp') and not first.operands[-1].mem.index \
        and (first.reg_name(first.operands[-1].mem.base) != 'sp' or first.operands[-1].mem.disp >= 0) and not RX18.search(first.op_str)
    slot = keyseq(start + 8)
    if len(decoded) != 9 or not safe or W(pat, start) != W(orig, prefix) \
            or W(orig, call) != 0xD63F0240 or W(pat, call) != 0xD63F03C0 \
            or W(pat, start + 4) != 0xF81F03E0 or W(pat, start + 8) != 0xD53BD07E \
            or not slot or slot[1] != KEY['K18'] \
            or W(pat, start + 20) != 0x8B0003DE or W(pat, start + 24) != 0xF94003DE \
            or W(pat, start + 28) != 0xF85F03E0 or btarget(start + 32) != call:
        bad(f'call window does not preserve store/scratch/link PC {prefix:#x}')
    elif decoded[3].reg_name(decoded[3].operands[0].reg) != 'x0' \
            or decoded[4].reg_name(decoded[4].operands[0].reg) != 'x0':
        bad(f'call window key scratch mismatch {prefix:#x}')

def paramseq(at, r):
    q = list(_md.disasm(pat[at:at + 40], at))
    tmp = 17 if r == 16 else 16
    rn = lambda ins, i: ins.reg_name(ins.operands[i].reg)
    if len(q) < 8 or q[0].mnemonic not in ('stur', 'str') or rn(q[0], 0) != f'x{tmp}' \
            or q[0].operands[1].mem.base != capstone.arm64_const.ARM64_REG_SP or q[0].operands[1].mem.disp not in (-16, -24) \
            or q[1].mnemonic != 'mrs' or rn(q[1], 0) != f'x{r}' \
            or 'tpidrro_el0' not in q[1].op_str or q[2].mnemonic != 'adrp' \
            or q[2].operands[1].imm != h['KEY_PAGE_OFF'] or q[3].mnemonic != 'ldr' \
            or q[3].operands[1].mem.base != q[2].operands[0].reg or q[4].mnemonic != 'add' \
            or q[4].operands[0].reg != q[1].operands[0].reg or q[4].operands[1].reg != q[1].operands[0].reg \
            or q[5].mnemonic != 'ldr' or q[5].operands[0].reg != q[1].operands[0].reg \
            or q[5].operands[1].mem.base != q[4].operands[0].reg or q[6].mnemonic not in ('ldur', 'ldr') \
            or rn(q[6], 0) != f'x{tmp}' \
            or q[6].operands[1].mem.base != capstone.arm64_const.ARM64_REG_SP \
            or q[6].operands[1].mem.disp != q[0].operands[1].mem.disp or q[7].mnemonic != 'b':
        return None
    d = q[3].operands[1].mem.disp
    return (q[6].address + 4, d // 8)
gates = {}
for l in open(f'{out_dir}/{mod}.gates.tsv'):
    r, k, arg, oa = l.split(); gates[int(r, 16)] = (k, int(arg), int(oa, 16))
def B(frm, to): return 0x14000000 | (((to - frm) >> 2) & 0x3FFFFFF)
# The gate block must have ldr x29,[sp] before blr: the x29 borrowed for fetching the slot via the key page is restored to the guest's original value, so the x29 the host reads is the real frame pointer (r26, see patch_nso emit_gate_call)
LDR_X29_SP = 0xF94003FD
gsites = sorted(a for a, k in sites.items() if k in ('svc', 'cntpct', 'cntvct'))
seen = set()
for a in gsites:
    s = btarget(a); ow = W(orig, a) if a not in _hostcall else (0xD4000001 | _hostcall[a] << 5)
    ks = keyseq(s + 4)
    if W(pat, s) != 0xA9BF7BFD or not ks or ks[1] != KEY.get('KGATE') \
            or W(pat, ks[0]) & 0xFFC00000 != 0xF9400000 or W(pat, ks[0] + 4) != LDR_X29_SP or W(pat, ks[0] + 8) != 0xD63F03C0 \
            or W(pat, ks[0] + 12) != 0xA8C17BFD or W(pat, ks[0] + 16) != B(ks[0] + 16, a + 4):
        bad(f'gate block does not match the key-page template {a:#x}'); continue
    arg = (ow >> 5) & 0xFFFF if sites[a] == 'svc' else ow & 31
    g = gates.get(ks[0] + 12)
    if g != (sites[a], arg, a): bad(f'gates.tsv mismatch {a:#x}: table {g} expected {(sites[a], arg, a)}')
    seen.add(ks[0] + 12)
# ⑦ Observation gates
import bisect as _bs
_starts = set(nsoimg.func_starts(orig, TE))
_busy = {a + i for a in list(sites) + list(inwin) for i in range(4)} | cutb
for off, oid, ow, why in obs:
    if W(orig, off) != ow: bad(f'observation gate {off:#x} original instruction word mismatch: image {W(orig, off):08x} registered {ow:08x}'); continue
    ins = next(_md.disasm(struct.pack('<I', ow), off), None)
    m = ins.mnemonic if ins else '?'
    if ins is None or m in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz', 'svc', 'mrs', 'msr', 'eret') or m.startswith('b.') \
       or (m.startswith('ldr') and '[' not in ins.op_str) or RX18.search(ins.op_str or ''):
        bad(f'observation gate {off:#x} original instruction cannot be relocated: {m} {ins.op_str if ins else ""}')
    # Function entry is no longer required: _dm_gate saves and restores x0–x17, x19–x30, q0–q31 and NZCV/FPSR/FPCR, so live registers in the middle of a function are unaffected;
    # this ABI is checked functionally by gatefn (the stub deliberately clobbers x9–x17 and NZCV, and full restoration is checked), not approximated here by position
    if set(range(off, off + 4)) & _busy: bad(f'observation gate {off:#x} overlaps a rewrite site / window / cut')
    s_ = btarget(off)
    if s_ is None or not (PO <= s_ < len(pat)): bad(f'observation gate head {off:#x} is not a B into the patch area'); continue
    ks = keyseq(s_ + 4)
    if W(pat, s_) != 0xA9BF7BFD or not ks or ks[1] != KEY.get('KGATE') \
            or W(pat, ks[0]) & 0xFFC00000 != 0xF9400000 or W(pat, ks[0] + 4) != LDR_X29_SP or W(pat, ks[0] + 8) != 0xD63F03C0 \
            or W(pat, ks[0] + 12) != 0xA8C17BFD or W(pat, ks[0] + 16) != ow \
            or W(pat, ks[0] + 20) != B(ks[0] + 20, off + 4):
        bad(f'observation gate block does not match the key-page template {off:#x}'); continue
    if gates.get(ks[0] + 12) != ('obs', oid, off): bad(f'observation gate row mismatch {off:#x}: table {gates.get(ks[0] + 12)}')
    seen.add(ks[0] + 12)
# ⑪ Branch gates introduced by cuts (hle.h registry form "branch gate: x8!=0 jump 0x<target>, otherwise run the original instruction and return 0x<original site>+4"): the original site is replaced with a B into the patch area, and the block equals word for word
#   stp x8,xzr,[sp,#-16]! ; stp x29,x30,[sp,#-16]! ; fetch KGATE via key page ; ldr x30,[x30] ; ldr x29,[sp] ; blr x30 ; ldp x29,x30 ; cbz w8,+12 ;
#   ldp x8,xzr ; b target ; ldp x8,xzr ; original instruction ; b original site+4 - x8 is restored to its original value on both paths, the host result is only used to pick the path; gates.tsv records (svc, number, original site)
nbranch = 0
for n, (roff, form) in _hreg.items():
    if not re.match(r'branch gate|\u5206\u652f\u95e8', form): continue   # also accept the older Chinese registry wording
    cut = next((c for c in cuts if c[0] == roff), None)
    if cut is None: continue                                       # This build output does not enable this private gate
    mt, mb = re.search(r'(?:\bjump|\u8df3) (0x[0-9a-fA-F]+)', form), re.search(r'(?:\breturn|\u56de) (0x[0-9a-fA-F]+)', form)
    if not mt or not mb or int(mb[1], 16) != roff + 4: bad(f'branch gate svc #{n:#x}: registry form lacks a jump target or the return point != original site+4: {form}'); continue
    tgt = int(mt[1], 16); s_ = btarget(roff); ks = keyseq(s_ + 8) if s_ is not None else None
    if s_ is None or not (PO <= s_ < len(pat)) or W(pat, s_) != 0xA9BF7FE8 or W(pat, s_ + 4) != 0xA9BF7BFD or not ks or ks[1] != KEY.get('KGATE') \
            or W(pat, ks[0]) & 0xFFC00000 != 0xF9400000 or W(pat, ks[0] + 4) != LDR_X29_SP or W(pat, ks[0] + 8) != 0xD63F03C0 or W(pat, ks[0] + 12) != 0xA8C17BFD \
            or W(pat, ks[0] + 16) != 0x34000068 or W(pat, ks[0] + 20) != 0xA8C17FE8 or W(pat, ks[0] + 24) != B(ks[0] + 24, tgt) \
            or W(pat, ks[0] + 28) != 0xA8C17FE8 or W(pat, ks[0] + 32) != W(orig, roff) or W(pat, ks[0] + 36) != B(ks[0] + 36, roff + 4):
        bad(f'branch gate svc #{n:#x} {roff:#x}: block does not match the template'); continue
    if gates.get(ks[0] + 12) != ('svc', n, roff): bad(f'branch gate svc #{n:#x} gates.tsv row mismatch: {gates.get(ks[0] + 12)}')
    seen.add(ks[0] + 12); nbranch += 1
# ⑫ Spin waits (patch_nso.py "Spin waits", registered builds only): each original back-edge became a B into the patch area (a conditional back-edge
#    through a two-word trampoline: the re-encoded conditional branch to the copy's head ; b back-edge+4); every word of the copy is an instruction of the
#    original loop region, or a re-encoded direct branch / adrp (its target inside the original function or inside this copy's block), never svc / x18 /
#    a system register; the gate block matches the template and its ldp is followed by b to the copy's head; gates.tsv has (spin, 0, head); back-edges
#    overlap no rewrite site / window / cut / observation gate / parameter gate
_BR = lambda w: w & 0xFC000000 == 0x14000000 or w & 0xFF000010 == 0x54000000 or w & 0x7E000000 in (0x34000000, 0x36000000)
def _brtarget(w, o):
    if w & 0xFC000000 == 0x14000000: return o + 4 * (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000)
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000: return o + 4 * ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000)
    if w & 0x7E000000 == 0x36000000: return o + 4 * ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000)
    return None
spin_gates = {r: oa for r, (k, arg, oa) in gates.items() if k == 'spin'}
_sbusy = _busy | obs_heads | par_words
nspin = 0
for T, bes in sorted(spins.items()):
    fs, fe = next(((a, b) for a, b in zip(sorted(_starts), sorted(_starts)[1:] + [TE]) if a <= T < b), (T, T))
    if not bes or any(not (T <= b < fe) for b in bes): bad(f'spin wait {T:#x}: a back-edge is not in the same function after the head'); continue
    rets = [r for r, oa in spin_gates.items() if oa == T]
    if len(rets) != 1: bad(f'spin wait {T:#x}: {len(rets)} gates.tsv spin rows (should be 1)'); continue
    r = rets[0]; ks = keyseq(r - 28)
    if W(pat, r - 32) != 0xA9BF7BFD or not ks or ks[1] != KEY.get('KGATE') or ks[0] != r - 12 or W(pat, r - 12) & 0xFFC00000 != 0xF9400000 \
            or W(pat, r - 8) != LDR_X29_SP or W(pat, r - 4) != 0xD63F03C0 or W(pat, r) != 0xA8C17BFD or btarget(r + 4) is None:
        bad(f'spin wait {T:#x}: the gate block does not match the key-page template'); continue
    copy0 = btarget(r + 4); gate0 = r - 32
    if not (PO <= copy0 < gate0): bad(f'spin wait {T:#x}: the b after the gate block does not go to this copy\'s head'); continue
    ok = True
    for b in bes:
        ob = W(orig, b)
        if set(range(b, b + 4)) & _sbusy: bad(f'spin wait {T:#x}: back-edge {b:#x} overlaps another rewrite'); ok = False; break
        if not _BR(ob) or _brtarget(ob, b) != T: bad(f'spin wait {T:#x}: original {b:#x} is not a direct branch back to the head'); ok = False; break
        t = btarget(b)
        if ob & 0xFC000000 == 0x14000000:
            if t != copy0: bad(f'spin wait {T:#x}: back-edge {b:#x} does not go to the copy\'s head'); ok = False; break
        elif t is None or not (PO <= t < len(pat)) or _brtarget(W(pat, t), t) != copy0 or (W(pat, t) ^ ob) & 0xFF00001F if ob & 0xFF000010 == 0x54000000 else False:
            bad(f'spin wait {T:#x}: the trampoline of conditional back-edge {b:#x} does not match'); ok = False; break
        elif W(pat, t + 4) != B(t + 4, b + 4): bad(f'spin wait {T:#x}: the second word of trampoline {t:#x} is not b {b + 4:#x}'); ok = False; break
    if not ok: continue
    hi = max(bes) + 28   # acq / rel / tst sequences may run past the last back-edge
    def _nextmatch(pos, pred):
        for a_ in range(pos, hi, 4):
            if pred(W(orig, a_)): return a_
        return None
    pos, prev_adrp = T, None
    for o in range(copy0, gate0, 4):
        w = W(pat, o)
        if w & 0xFC000000 == 0x14000000:   # b: an inserted fall-through or exit, or a copied b; only its target is checked
            t = _brtarget(w, o)
            if not (fs <= t < fe or copy0 <= t < r + 8 + 4 * 64): bad(f'spin wait {T:#x}: the b at copy {o:#x} targets {t:#x}, outside the original function and the copy block'); ok = False; break
            prev_adrp = None; continue
        if _BR(w):   # a conditional branch: the next original one of the same kind, condition and register (only the offset differs)
            m = 0xFFF8001F if w & 0x7E000000 == 0x36000000 else 0xFF00001F
            a_ = _nextmatch(pos, lambda ow: _BR(ow) and ow & 0xFC000000 != 0x14000000 and (ow ^ w) & m == 0)
            t = _brtarget(w, o)
            if a_ is None or not (fs <= t < fe or copy0 <= t < r + 8 + 4 * 64): bad(f'spin wait {T:#x}: the conditional branch at copy {o:#x} does not match the original loop'); ok = False; break
            pos = a_ + 4; prev_adrp = None; continue
        ins = next(_md.disasm(struct.pack('<I', w), o), None)
        if ins is not None and ins.mnemonic == 'adrp':   # rebuilt adr / adrp: the next original one into the same register
            a_ = _nextmatch(pos, lambda ow: ow & 0x1F000000 == 0x10000000 and ow & 31 == w & 31)
            if a_ is None: bad(f'spin wait {T:#x}: the adrp at copy {o:#x} does not match the original loop'); ok = False; break
            pos = a_ + 4; prev_adrp = (w & 31, W(orig, a_)); continue
        if ins is not None and ins.mnemonic == 'add' and prev_adrp and prev_adrp[1] & 0x80000000 == 0 and (w & 31) == prev_adrp[0]:   # adr rebuilt as adrp + add
            prev_adrp = None; continue
        a_ = _nextmatch(pos, lambda ow: ow == w)
        if a_ is None or (ins is not None and (ins.mnemonic in ('svc', 'mrs', 'msr') or RX18.search(ins.op_str or ''))):
            bad(f'spin wait {T:#x}: word {w:08x} at copy {o:#x} does not match the original loop in order'); ok = False; break
        pos = a_ + 4; prev_adrp = None
    if ok: seen.add(r); nspin += 1
if set(gates) != seen: bad(f'gates.tsv has extra rows {len(set(gates) - seen)}')

# ⑥ Thread slots
_base = {'K18', 'KTLS', 'KTLR', 'KGATE'}; _extra = set(KEY) - _base   # Extra keys = parameter-gate keys (KRES* / KASPECT* ...; the rewriter's global key table, listed in the headers of all three modules): must be uppercase names starting with K, with key numbers below the four base keys
if set(KEY) != {'K18', 'KTLS', 'KTLR', 'KGATE', 'KRES0', 'KRES1', 'KASPECT', 'KASPECTUI'} \
        or not {k for *_, k in params} <= set(KEY) or set(KEY.values()) != set(range(8)):
    bad(f'header key-page indices invalid: {KEY} (must have eight keys with indices exactly 0..7)')
kv = {v: k for k, v in KEY.items()}; nk = {}
for o in range(PO, len(pat) - 4, 4):
    w = W(pat, o)
    if w & 0xFFFFFFE0 != 0xD53BD060: continue           # mrs xN, tpidrro_el0
    n = w & 31; ks = keyseq(o); k = kv.get(ks[1]) if ks else None
    if not ks or k is None:
        bad(f'patch area {o:#x} reads tpidrro but is not followed by a key-page indirect slot sequence'); continue
    nk[k] = nk.get(k, 0) + 1
expected = {}
for kind in sites.values():
    key = {'x18': 'K18', 'W': 'K18', 'C': 'K18', 'tpidr_r': 'KTLS', 'tpidr_w': 'KTLS', 'tpidrro_r': 'KTLR'}.get(kind)
    if key: expected[key] = expected.get(key, 0) + 1
expected['KGATE'] = len(gsites) + len(obs) + nbranch + len(spins)   # one gate block per spin wait (⑫)
for _, _, _, key in params: expected[key] = expected.get(key, 0) + 1
if any(nk.get(k, 0) != v for k, v in expected.items()) or any(k not in expected and v for k, v in nk.items()):
    bad(f'key-page slot access count mismatch: expected {expected} actual {nk}')

# ⑧ Parameter gates (PARAMS, host only): a 64-bit constant originally built by movz xR,#lo / movk xR,#hi,lsl #32 is replaced at equal length by mrs xR,tpidrro_el0 / ldr xR,[xR,#key*8]
#   Checks: original word A is a 64-bit movz (hw=0) writing xR, original word B is a 64-bit movk (hw=2) writing xR; the new words = the template bit for bit; instructions between A and B do not read or write xR;
#   (A, B] is not the target of any direct branch nor a function entry (otherwise a path entering in the middle would get the slot pointer instead of the constant); the key number is registered in the header; no overlap with rewrite sites / cuts / observation gates.
#   When the host gives the slot the original value (packed width/height = the constants below), the semantics are bit-for-bit equivalent to the original two instructions: xR has the same value after B, and nobody reads xR between A and B.
par_const = {}
if params:
    import capstone
    _cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); _cs.detail = True
    _tg = set()
    for o in range(0, TE & ~3, 4):                                  # Direct branch targets across all of text
        w = W(orig, o)
        if w & 0x7C000000 == 0x14000000: d = ((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000; _tg.add(o + 4 * d)
        elif w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000: d = (((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000; _tg.add(o + 4 * d)
        elif w & 0x7E000000 == 0x36000000: d = (((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000; _tg.add(o + 4 * d)
    _fs = set(nsoimg.func_starts(orig, TE))
    _busy8 = {a for a in sites} | set(inwin) | obs_heads | {o & ~3 for o in cutb}
    for a, b, r, k in params:
        wa, wb = W(orig, a), W(orig, b)
        # Two kinds of original instruction pair: 64-bit movz xR,#lo / movk xR,#hi,lsl #32 (KRES*); 32-bit movz wR,#lo / movk wR,#hi,lsl #16 (KASPECT*, original value = hi<<16|lo,
        # writing a w register zeroes the upper 32 bits ⇒ the host slot must be stored as a u32 bit pattern with the upper 32 bits 0 (a runtime constraint that cannot be checked statically, see platform thread.c)
        w64 = wa & 0xFFE00000 == 0xD2800000 and wb & 0xFFE00000 == 0xF2C00000
        w32 = wa & 0xFFE00000 == 0x52800000 and wb & 0xFFE00000 == 0x72A00000
        if not (w64 or w32) or wa & 31 != r or wb & 31 != r:
            bad(f'parameter gate {a:#x}/{b:#x}: original instructions {wa:#010x}/{wb:#010x} are not a movz/movk constant pair (64-bit lsl #32 or 32-bit lsl #16) writing x{r}')
        if not a < b: bad(f'parameter gate {a:#x}/{b:#x} out of order')
        if k not in KEY: bad(f'parameter gate {a:#x} key {k} not registered in the header'); continue
        s = btarget(a); ks = paramseq(s, r) if s is not None else None
        ok = ks and ks[1] == KEY[k] and W(pat, b) == 0xD503201F
        if not ok: bad(f'parameter gate {a:#x}/{b:#x}: key-page indirect template mismatch')
        for o in range(a + 4, b, 4):
            ins = next(_cs.disasm(orig[o:o + 4], o), None)
            if ins is None: bad(f'parameter gate {a:#x}: cannot decode {o:#x} in between'); continue
            rr, ww = ins.regs_access()
            names = {ins.reg_name(x) for x in rr + ww}
            if f'x{r}' in names or f'w{r}' in names: bad(f'parameter gate {a:#x}: {o:#x} in between ({ins.mnemonic} {ins.op_str}) reads/writes x{r}')
        bt = sorted(t for t in _tg if a < t <= b) + sorted(t for t in _fs if a < t <= b)
        if bt: bad(f'parameter gate {a:#x}..{b:#x}: range contains branch targets / function entries {[hex(x) for x in bt[:4]]}')
        if {a, b} & _busy8: bad(f'parameter gate {a:#x}/{b:#x} overlaps a rewrite site / window / observation gate / cut')
        par_const[(a, k)] = ((wb >> 5) & 0xFFFF) << (16 if w32 else 32) | ((wa >> 5) & 0xFFFF)
    _kc = {}
    for (a, k), v in par_const.items(): _kc.setdefault(k, set()).add(v)
    for k, vs in _kc.items():
        if len(vs) != 1: bad(f'parameter gate key {k} maps to multiple original constants {[hex(v) for v in vs]} (one slot cannot hold a unique original value)')
    print('parameter gates ' + ', '.join(f'{a:#x}={k}:original {v:#x}' for (a, k), v in par_const.items()))
    # Downstream of a parameter register there must be no further masking by a constant: the compiler folds "the value can only be one of these constants" into later instructions - in the operation mode notification handler at 0xe10e48,
    # mov w9,#0x740; and x9,x25,x9 is exactly a mask derived from 1280|1600, and once replaced by a parameter the width gets masked to 768 / 64. From each parameter site, do taint propagation in linear order within the function
    # (mov / csel kinds propagate, other writes clear; branches are not followed, a conservative approximation); report when a tainted register meets and / ands / bic whose other operand is an immediate or a register built by a mov immediate
    _fsl = sorted(_fs)
    def _rn(ins, x): return re.sub(r'^[wx]', '', ins.reg_name(x)) if re.match(r'^[wx]\d+$', ins.reg_name(x) or '') else None
    _donef = set()
    for a, b, r, k in params:
        import bisect as _bi
        i = _bi.bisect_right(_fsl, a); f0 = _fsl[i - 1]; f1 = _fsl[i] if i < len(_fsl) else TE
        if f0 in _donef: continue
        _donef.add(f0)
        seeds = {(pa, str(pr)) for pa, pb, pr, pk in params if f0 <= pa < f1}
        taint, const = set(), {}
        for o in range(f0, f1, 4):
            for pa, pr in seeds:
                if o == pa: taint.add(pr)
            if any(o == pa for pa, _ in seeds): continue
            ins = next(_cs.disasm(pat[o:o + 4], o), None)
            if ins is None: continue
            rr, ww = ins.regs_access(); rd = {x for x in (_rn(ins, q) for q in rr) if x}; wr = {x for x in (_rn(ins, q) for q in ww) if x}
            m = ins.mnemonic
            if m in ('and', 'ands', 'bic', 'bics') and rd & taint:
                imm = any(op.type == capstone.arm64.ARM64_OP_IMM for op in ins.operands)
                if imm or (rd - taint) & set(const): bad(f'constant mask downstream of a parameter gate {o:#x}: {m} {ins.op_str} (function {f0:#x}, a value tainted by the parameter register is masked by a constant)')
            if m in ('mov', 'csel', 'csinc', 'csinv', 'csneg') and rd & taint: taint |= wr; [const.pop(x, None) for x in wr]
            else:
                taint -= wr
                isimm = m in ('mov', 'movz') and len(ins.operands) == 2 and ins.operands[1].type == capstone.arm64.ARM64_OP_IMM
                for x in wr:
                    if isimm: const[x] = ins.operands[1].imm
                    else: const.pop(x, None)

# ⑨ Static check of temporary registers: every non-original-instruction destination register in a patch block must have a matching red-zone save/restore pair at the same offset.
import capstone
def _writes(ins):
    _, ww = ins.regs_access()
    return {'x29' if ins.reg_name(r) == 'fp' else 'x30' if ins.reg_name(r) == 'lr' else re.sub(r'^[wx]', 'x', ins.reg_name(r))
            for r in ww if ins.reg_name(r) not in ('sp', 'pc')}
def _mem_sp(ins, load):
    if (load and not ins.mnemonic.startswith('ld')) or (not load and not ins.mnemonic.startswith('st')):
        return None
    for op in ins.operands:
        if op.type == capstone.arm64_const.ARM64_OP_MEM and ins.reg_name(op.mem.base) == 'sp':
            if op.mem.disp < 0: return op.mem.disp
            if load and ins.mnemonic == 'ldp' and op.mem.disp == 0: return -16
    return None
def _block(start):
    out = []
    for ins in _md.disasm(pat[start:start + 256], start):
        out.append(ins)
        if ins.mnemonic == 'b':
            t = ins.operands[0].imm
            if not (PO <= t < len(pat)): break
    return out
static_entries = [(a, set()) for a in sites]
for a, b, r, _ in params:
    static_entries.append((a, {f'x{r}'}))
static_seen = 0
for a, allowed in static_entries:
    s = btarget(a)
    if s is None: continue
    block = _block(s); static_seen += 1
    if not allowed:
        end = next((hi for lo, hi in wins if lo == a), a + 4)
        for lo, hi in atomic_groups.get(a, [(a, end)]):
            for ins in _md.disasm(orig[lo:hi], lo): allowed |= _writes(ins)
        if next(_md.disasm(orig[a:a + 4], a), None).mnemonic in ('stp', 'ldp'):
            allowed.update(('x0', 'x1'))
    saved = {}
    restored = {}
    for i, ins in enumerate(block):
        d = _mem_sp(ins, False)
        if d is not None:
            regs = [op.reg for op in ins.operands[:2] if op.type == capstone.arm64_const.ARM64_OP_REG]
            for reg in regs:
                name = ins.reg_name(reg); name = 'x29' if name == 'fp' else 'x30' if name == 'lr' else re.sub(r'^[wx]', 'x', name)
                saved.setdefault(name, []).append((d, i))
        d = _mem_sp(ins, True)
        if d is not None:
            regs = [op.reg for op in ins.operands[:2] if op.type == capstone.arm64_const.ARM64_OP_REG]
            for reg in regs:
                name = ins.reg_name(reg); name = 'x29' if name == 'fp' else 'x30' if name == 'lr' else re.sub(r'^[wx]', 'x', name)
                restored.setdefault(name, []).append((d, i))
    writes = set().union(*(_writes(ins) for ins in block)) if block else set()
    for reg in sorted(writes - allowed):
        wi = [i for i, ins in enumerate(block) if reg in _writes(ins) and not (i > 0 and _mem_sp(ins, True) is not None)]
        dead = any(ins.mnemonic in ('br', 'blr') and ins.operands and ins.reg_name(ins.operands[0].reg) in (reg, reg[1:]) for ins in block)
        ok = dead or any(sd == rd and si < min(wi, default=10**9) and ri > max(wi, default=-1)
                 for sd, si in saved.get(reg, []) for rd, ri in restored.get(reg, []))
        if not ok: bad(f'scratch register {a:#x}: block writes {reg}, outside the original destination registers and without a same-offset save/restore pair')
print(f'STATIC {mod} scanned={static_seen} violations={sum(1 for x in fails if x.startswith("scratch register"))}')

# ⑩ Trampoline exits (the handheld-switch crash: the parameter-gate trampoline returned to B+4, skipping the unmodified original instructions between A and B and another trampoline head):
#   for every b in text that jumps into the patch area (trampoline head a), follow the patch block through all exits back to text (sequential b and conditional branches; bl is treated as returning; ret / br do not count as returning to text).
#   Legal exits: a+4; the address after skipped words that are all words absorbed by the rewrite (≠ the original word and not another trampoline head, e.g. the nop at parameter-gate B);
#   window (W row [lo, hi)) heads: hi and the branch targets of the original instructions inside the window; non-window heads: the branch targets of the original instructions inside the contiguous absorbed span.
_heads = {o for o in range(0, TE & ~3, 4) if W(pat, o) & 0xFC000000 == 0x14000000 and btarget(o) is not None and btarget(o) >= PO}
_wend = dict(wins)
def _bt(w, o):
    if w & 0x7C000000 == 0x14000000: return o + 4 * (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000)
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000: return o + 4 * ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000)
    if w & 0x7E000000 == 0x36000000: return o + 4 * ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000)
    return None
if calls:
    second_words = set(calls.values())
    for address in range(0, TE, 4):
        if _bt(W(orig, address), address) in second_words:
            bad(f'known branch enters call without preparation {address:#x}')
    if second_words & set(nsoimg.func_starts(orig, TE)):
        bad('function entry enters call without preparation')
    for _, typ, _, value, addend in nsoimg.relocs(orig):
        if (typ == 1027 and addend in second_words) or value in second_words:
            bad('relocation enters call without preparation')

def _exits(start):
    ex, seen, st = set(), set(), [start]
    while st:
        o = st.pop()
        while o not in seen and o + 4 <= len(pat):
            seen.add(o); w = W(pat, o)
            if w & 0xFC000000 == 0x14000000:
                t = _bt(w, o)
                if t < PO: ex.add(t); break
                o = t; continue
            if w & 0xFC000000 == 0x94000000: o += 4; continue
            if w == 0xD65F03C0 or w & 0xFFFFFC1F == 0xD61F0000: break
            t = _bt(w, o)
            if t is not None:
                if t < PO: ex.add(t)
                else: st.append(t)
            o += 4
    return ex
nexit = 0; _cutoff = {c[0] for c in cuts}; _cutex = []
_spin_of = {b: T for T, bes in spins.items() for b in bes}
for a in sorted(_heads):
    if a in _spin_of:   # a spin wait's back-edge (⑫): its copy leaves to addresses of the loop region and to their original branch targets
        T = _spin_of[a]; seg = range(T, max(spins[T]) + 28, 4)   # acq / rel / tst sequences may run past the last back-edge
        legal = set(range(T, max(spins[T]) + 32, 4))
    elif a in _wend:
        group = atomic_groups.get(a, [(a, _wend[a])])
        seg = [o for lo, hi in group for o in range(lo, hi, 4)]
        legal = {calls[a]} if a in calls else {hi for lo, hi in group}
    else:
        e = a + 4
        while e < TE and W(pat, e) != W(orig, e) and e not in _heads: e += 4
        seg = range(a, e, 4); legal = {a + 4 * k for k in range(1, len(seg) + 1)}
    legal |= {t for o in seg for t in [_bt(W(orig, o), o)] if t is not None}
    for t in _exits(btarget(a)):
        nexit += 1
        if t not in legal and a in _cutoff: _cutex.append(f'{a:#x}→{t:#x}'); continue   # Registered cuts (with a reason, deliberately changing control flow): let through and listed
        if t not in legal: bad(f'trampoline exit {a:#x} -> {t:#x}: neither {a + 4:#x} / absorbed-run end / window end, nor a branch target of an original instruction in the run (skips an unchanged original instruction or another trampoline head)')
print(f'EXITS {mod} trampoline_heads={len(_heads)} exits={nexit} registered_cut_control_flow_exits={_cutex}')

cnt = {}
if _hostcall: print(f'host-call gates introduced by cuts: {len(_hostcall)}: ' + ', '.join(f"{a:#x}=svc#{n:#x}" for a, n in _hostcall.items()))
for k in sites.values(): cnt[k] = cnt.get(k, 0) + 1
print(f'STRUCT {mod} {"OK" if not fails else "FAIL"} spin_waits={nspin}/{len(spins)} sites={len(sites)} {cnt} jump_tables={jumps} window_x18_refs={nwin_ref} '
      f'registered_cuts={len(cuts)}(code{ncut_code}/data{ncut_data}) observation_gates={len(obs)} parameter_gates={len(params)} '
      f'independent_denominator={len(ind)} text_changed_words={len(diff)} gates={len(gsites)} slot_accesses={nk} failures={len(fails)}')
sys.exit(1 if fails else 0)
