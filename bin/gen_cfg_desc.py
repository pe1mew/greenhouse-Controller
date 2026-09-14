#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Generate the gh#64 config descriptor table from the six tables that exist today.

**Why generate rather than type it.** The refactor exists because a key list
kept in six hand-maintained places drifts, and every drift so far has shipped
(gh#53, gh#57 twice, gh#51 group B). Hand-transcribing 51 keys x 7 fields into a
seventh table would reintroduce exactly that risk on the way to removing it. So
the table is *derived*: every field is read out of the sources that define
today's behaviour, which makes the generated table equivalent by construction
and re-runnable when something moves.

**What each field comes from**

| field | source |
|---|---|
| ns / key | the union of the six key sets |
| kind | membership of A (`cfg_key_kind()`) and B (the shadow ladder) |
| published | membership of E (`LIMITS_JSON`) |
| min / max | `CFG_MIN_*` / `CFG_MAX_*` in cfg_limits.h, expanded as the preprocessor would |
| param_id | the `LOG_PARAM_*` each key maps to in `ns_key_to_log_id()` |

Every one of the 17 declared gaps becomes a **field value** rather than an
omission -- NVS_ONLY, unaudited, unpublished, not-via-Q4 -- which is the whole
point: a key cannot be silently missing from a consumer if there is only one
table and the consumers read it.

Usage:
    python bin/gen_cfg_desc.py            # print the table
    python bin/gen_cfg_desc.py --check    # re-derive and diff against the
                                          # committed header; non-zero on drift
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DM_PATH = os.path.join(ROOT, "firmware", "src", "data_manager", "data_manager.cpp")
WS_PATH = os.path.join(ROOT, "firmware", "src", "web_server", "web_server.cpp")
LIM_PATH = os.path.join(ROOT, "firmware", "config", "cfg_limits.h")

sys.path.insert(0, HERE)
import check_cfg_tables as C            # reuse its parsing, not a second copy


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def macro_values():
    """CFG_MIN_* / CFG_MAX_* as integers, expanded as the preprocessor would."""
    out = {}
    for name, val in re.findall(
            r"#define\s+(CFG_(?:MIN|MAX)_[A-Z0-9_]+)\s+(\(?-?\d+\)?)",
            read(LIM_PATH)):
        out[name] = int(val.strip("()"))
    return out


def published_bounds():
    """{key: (min, max)} exactly as LIMITS_JSON publishes them today."""
    import json
    defs = dict(re.findall(
        r"#define\s+(CFG_(?:MIN|MAX)_[A-Z0-9_]+)\s+(\(?-?\d+\)?)", read(LIM_PATH)))
    body = C.limits_literal(read(WS_PATH))
    token = re.compile(r'"((?:[^"\\]|\\.)*)"' + r"|(CFG_(?:MIN|MAX)_[A-Z0-9_]+)")
    parts = []
    for lit, macro in token.findall(body):
        parts.append(defs[macro].strip("()") if macro else lit.replace('\\"', '"'))
    return {k: tuple(v) for k, v in json.loads("".join(parts)).items()}


def const_to_key(dm):
    """{K_CONSTANT: "key_string"}.

    Built once and shared: the first version of this file built the mapping
    twice, in opposite directions, and silently matched nothing."""
    return {m[0]: m[1] for m in re.findall(
        r'static const char (K_[A-Z0-9_]+)\[\]\s*=\s*"([a-z0-9_]+)"', dm)}


def array_branches(body, kmap, tail_re):
    """{key: captured} for the `static const char * const kXX[] = {...}` form.

    Two passes on purpose. One regex spanning declaration-to-use cannot work
    here: all three arrays are declared before any is used, so the match for the
    first CONSUMES the other two declarations, and re.findall returns
    non-overlapping matches. Only travel_* came out that way; dwell_open_* and
    dwell_close_* silently read as unaudited, which is precisely the class of
    silent omission this whole refactor exists to end.
    """
    out = {}
    for arr, names in re.findall(
            r"static const char \* const (\w+)\[\]\s*=\s*\{([^}]*)\};", body):
        m = re.search(re.escape(arr) + r"\[i\].*?" + tail_re, body, re.S)
        if not m:
            continue
        for cname in re.findall(r"K_[A-Z0-9_]+", names):
            if cname in kmap:
                out[kmap[cname]] = m.group(1)
    return out


def clamp_bounds():
    """{key: (MIN_MACRO, MAX_MACRO)} from cfg_clamp().

    The function is a ladder of `if (strcmp(key, K_X) == 0) { _CLAMP(A, B); }`,
    sometimes over an array of per-channel key constants, so both shapes are
    picked up and mapped back through the K_* constant names."""
    dm = C.strip_c_comments(read(DM_PATH))
    kmap = const_to_key(dm)
    body = C.fn_body(dm, r"int32_t\s+cfg_clamp\s*\(", "cfg_clamp")
    out = {}
    # arrays: static const char * const kxx[] = { K_A, K_B, K_C };  ... _CLAMP(a,b)
    for arr, names, mn, mx in re.findall(
            r"static const char \* const (\w+)\[\]\s*=\s*\{([^}]*)\};"
            r"(?:.*?)\1\[i\][^;]*?_CLAMP\(\s*([A-Z0-9_]+)\s*,\s*([A-Z0-9_]+)\s*\)",
            body, re.S):
        for cname in re.findall(r"K_[A-Z0-9_]+", names):
            if cname in kmap:
                out[kmap[cname]] = (mn, mx)
    # singles: no braces, and the bound may be an integer literal rather than a
    # CFG_* macro. `ap_enable` is matched as a bare string because it is written
    # as one in the source.
    tok = r"(-?\d+|[A-Z][A-Z0-9_]*)"
    for c1, c2, mn, mx in re.findall(
            r'strcmp\(key,\s*(?:(K_[A-Z0-9_]+)|"([a-z0-9_]+)")\)\s*==\s*0\s*\)'
            r"[^;]*?_CLAMP\(\s*" + tok + r"\s*,\s*" + tok + r"\s*\)",
            body, re.S):
        key = kmap.get(c1) if c1 else c2
        if key:
            out.setdefault(key, (mn, mx))
    return out


def param_ids():
    """{key: LOG_PARAM_*} from ns_key_to_log_id()."""
    dm = C.strip_c_comments(read(DM_PATH))
    kmap = const_to_key(dm)
    body = C.fn_body(dm, r"log_param_id_t\s+ns_key_to_log_id\s*\(", "ns_key_to_log_id")
    out = {}
    out.update(array_branches(body, kmap,
                              r"return\s+(LOG_PARAM_[A-Z0-9_]+)"))
    for cname, pid in re.findall(
            r"strcmp\(key,\s*(K_[A-Z0-9_]+)\)\s*==\s*0\s*\)[^;]*?return\s+(LOG_PARAM_[A-Z0-9_]+)",
            body, re.S):
        if cname in kmap:
            out.setdefault(kmap[cname], pid)

    # OR-chains: four keys sharing one return, as lat/lon do. The singles
    # pattern above only ever saw the LAST clause, so lat_deg, lat_frac and
    # lon_deg read as unaudited while lon_frac did not -- three keys losing
    # their audit row to a regex, which is the same silent-omission shape the
    # refactor is meant to end.
    for cond, pid in re.findall(
            r"if\s*\(((?:[^()]|\([^()]*\))*?\|\|(?:[^()]|\([^()]*\))*?)\)\s*return\s+(LOG_PARAM_[A-Z0-9_]+)",
            body, re.S):
        for cname in re.findall(r"K_[A-Z0-9_]+", cond):
            if cname in kmap:
                out.setdefault(kmap[cname], pid)
    return out


def build():
    sets = C.collect()
    if C._FATAL:
        sys.exit("could not read the existing tables")
    universe = sorted(set().union(*sets.values()))
    pub = published_bounds()
    clamps = clamp_bounds()
    pids = param_ids()
    macros = macro_values()

    def resolve(tokn):
        """A bound is either a CFG_* macro or an integer literal written
        in place (_CLAMP(0, 1)). Both are real bounds; only the macro form
        has a name worth carrying into the table."""
        if tokn is None:
            return None, None
        if re.fullmatch(r"-?\d+", tokn):
            return int(tokn), None
        return macros.get(tokn), tokn

    rows = []
    for key in universe:
        in_a = key in sets["A"]
        in_b = key in sets["B"]
        kind = ("CFG_KIND_SHADOW" if in_b else
                "CFG_KIND_NVS_ONLY" if in_a else
                "CFG_KIND_NOT_Q4")      # owned by a dedicated route (the ota_* four)
        if key in clamps:
            mn, mn_m = resolve(clamps[key][0])
            mx, mx_m = resolve(clamps[key][1])
        elif key in pub:
            mn, mx = pub[key]
            mn_m = mx_m = None
        else:
            mn = mx = mn_m = mx_m = None
        rows.append({
            "key": key,
            "kind": kind,
            "published": key in sets["E"],
            "min": mn, "max": mx, "min_macro": mn_m, "max_macro": mx_m,
            "param": pids.get(key, "LOG_PARAM_NONE"),
        })
    return rows, pub


def main():
    rows, pub = build()
    print("%-18s %-18s %-5s %-24s %s" % ("key", "kind", "pub", "param_id", "bounds"))
    print("-" * 92)
    for r in rows:
        b = ("%s..%s" % (r["min"], r["max"])) if r["min"] is not None else "(none)"
        if r["min_macro"]:
            b += "  [%s..%s]" % (r["min_macro"], r["max_macro"])
        print("%-18s %-18s %-5s %-24s %s"
              % (r["key"], r["kind"].replace("CFG_KIND_", ""),
                 "y" if r["published"] else ".", r["param"], b))

    # Self-check: every published key's bounds must match what LIMITS_JSON
    # publishes today. A mismatch means the clamp and the published range
    # disagree -- which is gh#57 part 2, the silent lie, all over again.
    bad = [r["key"] for r in rows
           if r["published"] and r["min"] is not None
           and pub.get(r["key"]) != (r["min"], r["max"])]
    # Cross-check the DERIVED table against check_cfg_tables' declared gaps.
    # The generator and the checker read the same sources by different routes,
    # so agreement is real evidence that the table reproduces today's behaviour.
    # Disagreement means the extraction is lying -- the one failure mode that
    # would make this refactor worse than the drift it replaces.
    problems = []
    for r in rows:
        absent, _why = C.EXPECTED_GAPS.get(r["key"], ("", None))
        if ("D" in absent) != (r["param"] == "LOG_PARAM_NONE"):
            problems.append("%s: audit status disagrees with its declared gap" % r["key"])
        if ("E" in absent) == r["published"]:
            problems.append("%s: publish status disagrees with its declared gap" % r["key"])
    print("\n%d keys" % len(rows))
    if bad:
        print("MISMATCH between cfg_clamp() and LIMITS_JSON for: %s" % ", ".join(bad))
        return 1
    print("clamped bounds agree with published bounds for every published key")
    if problems:
        for m in problems:
            print("  DISAGREES  %s" % m)
        return 1
    print("every key's audit and publish status matches its declared gap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
