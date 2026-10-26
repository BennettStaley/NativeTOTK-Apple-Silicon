# D2R rewriter source

Upstream: https://github.com/jhaizhou-ops/d2r-mobile

Pinned commit: `9f0c25768ac4b231c6f4cb0a9f182ded68d0e7cf`

Copied files: `dm/tools/patch_nso.py`, `dm/verify/structck.py`,
`dm/verify/equiv.py`, `dm/verify/nsoimg.py`. MIT license: see LICENSE in this directory.
`call_window.py` is a call-window encoder added by this project.

Local changes:

1. When recognizing `br x18` jump tables, the look-back window grows from 8 to 32 instructions.
   In Tears of the Kingdom v0, `main+0x590574` has floating-point instructions between the address
   computation and the jump, so the old eight-instruction window could not reach the `adr x18` at `main+0x590550`.
2. A two-word C window for `main+0x9c9858: blr x18`: it replays the preceding stack store,
   saves/restores the borrowed registers, prepares x30 through the K18 thread slot, then returns to the
   original call site to execute `blr x30`. The original SP, arguments and the PC that produces LR therefore
   stay the same. Known direct branches, function entries, dynamic definitions and relative relocations are
   checked, and entering the second word while bypassing the preparation word is rejected.
3. The first entry of the exclusive-window exit table becomes the default fall-through exit hi, fixing two
   external-branch windows that fell into the taken exit when their condition did not hold.
4. The structure checker understands C windows and checks their template and exits instruction by instruction;
   without game-specific host gates it does not require D2R's hle.h, while CUTS that involve host gates still
   need the registered interfaces given explicitly.
5. The equivalence checker continues executing the original call site's BLR inside a C window, then compares
   the callee's entry state. The other criteria are unchanged; exclusive accesses still use the existing
   deexcl test model.

6. Spin waits (`patch_nso.py` "Spin waits"): the loop search and copy ported from nxnative
   (https://github.com/seedds/nxnative, MIT, commits 066f2af and 6d34984; licence in
   `games/totk/third_party/licenses/nxnative.txt`). Only the build IDs registered in `SPIN_BUILDS` are rewritten (currently
   Breath of the Wild 1.9.0 main); there is no in-code data table, so the loop must lie within one eh_frame function; words
   already rewritten are recognised by their difference from the decoded image (`IMG0`); the gate is this project's
   `runtime_gate`, and the host side is `runtime_spin_wait` in `threads.c`. Structure check ⑫ verifies the back-edges,
   trampolines, copies and gate blocks.

main/sdk/subsdk0 have all been regenerated with the local version and passed full structure checks and
per-site equivalence checks. The new call form also has recursion, exception-target, negative-control and
M4 native multithreaded tests; the fixed exclusive window also has tests of the unsimplified LDXR/STXR
success path and an injected failure status.
To reproduce: `make verify`; the project README and claude.md describe the full scope.

`../../runtime/gate.S` adapts the register-preserving `dm_gate` section of
`dm/host/entry.S` from the same pinned upstream commit, under the included MIT
license. The loader and service dispatch are separate TOTK-specific code.
