"""REL-chunk call/jump inline-cache transform (port of bench-out/apply-relcall.pl).

Same contract as the DOL icall pass, but resolution goes through
mg_lookup_fn (DOL table + REL chunk table + retail-alias twinning) and the
site cache is pinned by BOTH epochs: g_mg_dcache_gen (dispatch verdicts /
SMC / demotion) and g_rel_dispatch_gen (module link/unlink: a cached REL fn
is only valid while its owner stays linked).

Transforms, in every chunks/chunk_*.c under the given rels dir:
  call: ctx->lr = 0xCONTu; ctx->pc = (0xTARGu|target); return;
  jump: ctx->pc = target; return;             (bctr tail calls)
Claimed dispatches publish their resolved (pc, fn) via g_rel_dispatched_*;
the site memoizes that pair, so retail-alias calls replay as direct calls
with the L twin patched in. E4-gate pcs never publish (the loader keeps them
unmemoizable), so they can never enter a site cache.

The emitted text matches the Perl original byte for byte (LF line endings),
so modules built from either stay interchangeable.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

CALL = re.compile(r"ctx->lr = 0x([0-9A-Fa-f]+)u;\n([ \t]+)ctx->pc = (0x[0-9A-Fa-f]+u|target);\n\2return;")
JUMP = re.compile(r"ctx->pc = target;\n([ \t]+)return;")


def _call_block(t, cont, ind, targ):
    lines = [
        f"ctx->lr = 0x{cont}u;",
        f"{ind}ctx->pc = {targ};",
        f"{ind}{{",
        f"{ind}    static u32 s_ict_{t}, s_icp_{t}, s_icdg_{t}, s_icrg_{t}, s_icg2_{t};",
        f"{ind}    static DolRecompFunction s_icf_{t};",
        f"{ind}    if (s_ict_{t} == {targ} && s_icdg_{t} == g_mg_dcache_gen && s_icrg_{t} == g_rel_dispatch_gen) {{",
        f"{ind}        ctx->pc = s_icp_{t};",
        f"{ind}        if (dolrecomp_call_enter()) {{",
        f"{ind}            s_icf_{t}(ctx);",
        f"{ind}            dolrecomp_call_leave();",
        f"{ind}            if (ctx->pc == 0x{cont}u) {{",
        f"{ind}                if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) return;",
        f"{ind}                if (s_icg2_{t} != g_mg_dcache_gen) {{",
        f"{ind}                    if (!(g_mg_native_ok && g_mg_native_ok(0x{cont}u, g_mg_native_ok_user)))",
        f"{ind}                        return;",
        f"{ind}                    s_icg2_{t} = g_mg_dcache_gen;",
        f"{ind}                }}",
        f"{ind}                goto label_{cont};",
        f"{ind}            }}",
        f"{ind}        }}",
        f"{ind}        return;",
        f"{ind}    }}",
        f"{ind}    if (dolrecomp_dispatch_replacement(ctx, {targ})) {{",
        f"{ind}        if (g_rel_dispatched_fn) {{",
        f"{ind}            s_ict_{t} = {targ}; s_icp_{t} = g_rel_dispatched_pc; s_icf_{t} = g_rel_dispatched_fn;",
        f"{ind}            s_icdg_{t} = g_mg_dcache_gen; s_icrg_{t} = g_rel_dispatch_gen;",
        f"{ind}        }}",
        f"{ind}        if (ctx->pc == 0x{cont}u) {{",
        f"{ind}            if (s_icg2_{t} != g_mg_dcache_gen) {{",
        f"{ind}                if (!(g_mg_native_ok && g_mg_native_ok(0x{cont}u, g_mg_native_ok_user)))",
        f"{ind}                    return;",
        f"{ind}                s_icg2_{t} = g_mg_dcache_gen;",
        f"{ind}            }}",
        f"{ind}            goto label_{cont};",
        f"{ind}        }}",
        f"{ind}        return;",
        f"{ind}    }}",
        f"{ind}    {{",
        f"{ind}        u32 rpc_{t};",
        f"{ind}        DolRecompFunction f_{t} = mg_lookup_fn({targ}, &rpc_{t});",
        f"{ind}        if (f_{t} && (!g_mg_native_ok || g_mg_native_ok(rpc_{t}, g_mg_native_ok_user))) {{",
        f"{ind}            s_ict_{t} = {targ}; s_icp_{t} = rpc_{t}; s_icf_{t} = f_{t};",
        f"{ind}            s_icdg_{t} = g_mg_dcache_gen; s_icrg_{t} = g_rel_dispatch_gen;",
        f"{ind}            ctx->pc = rpc_{t};",
        f"{ind}            if (dolrecomp_call_enter()) {{",
        f"{ind}                f_{t}(ctx);",
        f"{ind}                dolrecomp_call_leave();",
        f"{ind}                if (ctx->pc == 0x{cont}u) {{",
        f"{ind}                    if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) return;",
        f"{ind}                    if (s_icg2_{t} != g_mg_dcache_gen) {{",
        f"{ind}                        if (!(g_mg_native_ok && g_mg_native_ok(0x{cont}u, g_mg_native_ok_user)))",
        f"{ind}                            return;",
        f"{ind}                        s_icg2_{t} = g_mg_dcache_gen;",
        f"{ind}                    }}",
        f"{ind}                    goto label_{cont};",
        f"{ind}                }}",
        f"{ind}            }}",
        f"{ind}        }}",
        f"{ind}    }}",
        f"{ind}    return;",
        f"{ind}}}",
    ]
    return "\n".join(lines)


def _jump_block(t, ind):
    lines = [
        "ctx->pc = target;",
        f"{ind}{{",
        f"{ind}    static u32 s_ijt_{t}, s_ijp_{t}, s_ijdg_{t}, s_ijrg_{t};",
        f"{ind}    static DolRecompFunction s_ijf_{t};",
        f"{ind}    if (s_ijt_{t} == target && s_ijdg_{t} == g_mg_dcache_gen && s_ijrg_{t} == g_rel_dispatch_gen) {{",
        f"{ind}        ctx->pc = s_ijp_{t};",
        f"{ind}        if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) return;",
        f"{ind}        if (dolrecomp_call_enter()) {{",
        f"{ind}            s_ijf_{t}(ctx);",
        f"{ind}            dolrecomp_call_leave();",
        f"{ind}        }}",
        f"{ind}        return;",
        f"{ind}    }}",
        f"{ind}    if (dolrecomp_dispatch_replacement(ctx, target)) {{",
        f"{ind}        if (g_rel_dispatched_fn) {{",
        f"{ind}            s_ijt_{t} = target; s_ijp_{t} = g_rel_dispatched_pc; s_ijf_{t} = g_rel_dispatched_fn;",
        f"{ind}            s_ijdg_{t} = g_mg_dcache_gen; s_ijrg_{t} = g_rel_dispatch_gen;",
        f"{ind}        }}",
        f"{ind}        return;",
        f"{ind}    }}",
        f"{ind}    {{",
        f"{ind}        u32 rjp_{t};",
        f"{ind}        DolRecompFunction fj_{t} = mg_lookup_fn(target, &rjp_{t});",
        f"{ind}        if (fj_{t} && (!g_mg_native_ok || g_mg_native_ok(rjp_{t}, g_mg_native_ok_user))) {{",
        f"{ind}            s_ijt_{t} = target; s_ijp_{t} = rjp_{t}; s_ijf_{t} = fj_{t};",
        f"{ind}            s_ijdg_{t} = g_mg_dcache_gen; s_ijrg_{t} = g_rel_dispatch_gen;",
        f"{ind}            ctx->pc = rjp_{t};",
        f"{ind}            if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) return;",
        f"{ind}            if (dolrecomp_call_enter()) {{",
        f"{ind}                fj_{t}(ctx);",
        f"{ind}                dolrecomp_call_leave();",
        f"{ind}            }}",
        f"{ind}        }}",
        f"{ind}    }}",
        f"{ind}    return;",
        f"{ind}}}",
    ]
    return "\n".join(lines)


def transform_source(src: str) -> tuple[str, int, int, int]:
    """Return (new_source, calls, jumps, skipped_calls) for one chunk file."""
    orig = src  # labels never change during the transform
    n = calls = jumps = skipped = 0

    def call(m):
        nonlocal n, calls, skipped
        cont, ind, targ = m.group(1).upper(), m.group(2), m.group(3)
        if f"\nlabel_{cont}:" not in orig:
            skipped += 1
            return f"ctx->lr = 0x{cont}u;\n{ind}ctx->pc = {targ};\n{ind}return;"
        n += 1
        calls += 1
        return _call_block(f"rc{n}", cont, ind, targ)

    def jump(m):
        nonlocal n, jumps
        n += 1
        jumps += 1
        return _jump_block(f"rc{n}", m.group(1))

    src = CALL.sub(call, src)
    src = JUMP.sub(jump, src)
    return src, calls, jumps, skipped


def transform_tree(rels_root: Path) -> dict:
    totals = dict(files=0, calls=0, jumps=0, skipped=0)
    for module in sorted(p for p in rels_root.iterdir() if (p / "chunks").is_dir()):
        for chunk in sorted((module / "chunks").glob("chunk_*.c")):
            src = chunk.read_text(encoding="utf-8")  # universal newlines
            new, calls, jumps, skipped = transform_source(src)
            totals["skipped"] += skipped
            if calls or jumps:
                chunk.write_text(new, encoding="utf-8", newline="\n")
                totals["files"] += 1
                totals["calls"] += calls
                totals["jumps"] += jumps
    return totals


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: relcall.py <rels-root>")
    print(transform_tree(Path(sys.argv[1])))
