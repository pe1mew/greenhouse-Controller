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

**This script reads the six tables, which no longer exist.** Collapsing them is
what gh#64 did, and this is what produced the table that replaced them. So on a
current tree it must be pointed at the revision that still had them:

    python bin/gen_cfg_desc.py --from-rev ':/tools.gh#64' --check

`:/text` is git's search-by-message syntax, used deliberately in place of a
literal hash: this commit gets cherry-picked between `main` and `ropeSensor`,
which rewrites its hash, and a pinned hash would quietly stop resolving on
whichever branch did not originate it. (`.` not `\(` because :/ takes a regex.)

That is not a formality. It is the standing proof that the descriptor still
reproduces what the six tables did -- re-runnable by anyone, at any time,
rather than a claim in a commit message. For checking the descriptor itself
(bounds, shadow fields, the mock server) use bin/check_cfg_desc.py, which runs
against HEAD and is what the pre-commit hook calls.

Usage:
    python bin/gen_cfg_desc.py --from-rev ':/tools.gh#64'            # print the table
    python bin/gen_cfg_desc.py --from-rev ':/tools.gh#64' --check    # diff against the
                                                          # committed .inc
    python bin/gen_cfg_desc.py --from-rev ':/tools.gh#64' --emit     # (re)write it
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
    # A bound literal may be decimal OR hex: status_expose is clamped
    # _CLAMP(0, 0x3F) because it is a bitmask. The first version of this regex
    # accepted only decimal, so status_expose came out with NO bounds at all --
    # and nothing noticed, because the two self-checks below only tested audit
    # and publish status. That is why check 3 (every key must have bounds)
    # exists: a key the write path does not range-check is gh#57 part 1.
    tok = r"(-?\d+|0[xX][0-9a-fA-F]+|[A-Z][A-Z0-9_]*)"
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


NS_CONSTS = ("NVS_NS_CLIMATE", "NVS_NS_WIND", "NVS_NS_MOTOR",
             "NVS_NS_WIFI", "NVS_NS_SYSTEM")


def ns_sections(body):
    """[(NS_CONST, section_text)] by splitting on the `strcmp(ns, NVS_NS_X)` arms.

    Every key lives inside exactly one namespace arm, so the enclosing arm IS
    the key's namespace -- there is no separate table to consult and therefore
    none to drift."""
    marks = [(m.start(), m.group(1)) for m in
             re.finditer(r"strcmp\(ns,\s*(NVS_NS_[A-Z]+)\)\s*==\s*0", body)]
    out = []
    for i, (pos, nsc) in enumerate(marks):
        stop = marks[i + 1][0] if i + 1 < len(marks) else len(body)
        out.append((nsc, body[pos:stop]))
    return out


def key_namespaces():
    """{key: NVS_NS_*} read off cfg_clamp()'s namespace arms.

    cfg_clamp() is the one table that carries all 51 keys -- including the four
    ota_* that never reach Q4 -- so it is the only complete source for this."""
    dm = C.strip_c_comments(read(DM_PATH))
    kmap = const_to_key(dm)
    body = C.fn_body(dm, r"int32_t\s+cfg_clamp\s*\(", "cfg_clamp")
    out = {}
    for nsc, sec in ns_sections(body):
        for cname in re.findall(r"K_[A-Z0-9_]+", sec):
            if cname in kmap:
                out.setdefault(kmap[cname], nsc)
        for lit in re.findall(r'strcmp\(key,\s*"([a-z0-9_]+)"\)', sec):
            out.setdefault(lit, nsc)
    return out


def shadow_fields():
    """{key: (field_expr, needs_sun_recalc)} from apply_config_update()'s ladder.

    Derived, not typed, for the same reason as everything else here: the ladder
    is the fifth hand-maintained copy of the key set, and the whole point of the
    descriptor is that adding a key is ONE edit. `field_expr` is written the way
    C wants it -- `travel_s[0]` for the per-channel arrays -- so the emitted
    offsetof()/sizeof() pair cannot disagree with each other or with the name.
    """
    dm = C.strip_c_comments(read(DM_PATH))
    kmap = const_to_key(dm)
    body = C.fn_body(dm, r"static bool\s+apply_config_update\s*\(",
                     "apply_config_update")
    out = {}

    # Per-channel arrays first. Same non-overlap care as array_branches(): the
    # three declarations are consecutive, so `[^}]*` must not be allowed to
    # cross a closing brace.
    arrmap = {}
    for arr, names in re.findall(
            r"static const char \* const (\w+)\[\]\s*=\s*\{([^}]*)\};", body):
        arrmap[arr] = [kmap[c] for c in re.findall(r"K_[A-Z0-9_]+", names)
                       if c in kmap]
    for arr, arm in re.findall(
            r"strcmp\(key_str,\s*(\w+)\[i\]\)\s*==\s*0\)\s*\{([^}]*)\}", body):
        m = re.search(r"s_cfg\.(\w+)\[i\]\s*=", arm)
        if not m or arr not in arrmap:
            continue
        for idx, key in enumerate(arrmap[arr]):
            out[key] = ("%s[%d]" % (m.group(1), idx), False)

    # Singles.
    for cname, arm in re.findall(
            r"strcmp\(key_str,\s*(K_[A-Z0-9_]+)\)\s*==\s*0\)\s*\{([^}]*)\}", body):
        m = re.search(r"s_cfg\.(\w+)\s*=\s*v(?:16|32)\s*;", arm)
        if m and cname in kmap:
            out.setdefault(kmap[cname],
                           (m.group(1), "update_sun_times" in arm))
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
    nsmap = key_namespaces()
    shadow = shadow_fields()
    kconsts = {v: k for k, v in
               const_to_key(C.strip_c_comments(read(DM_PATH))).items()}

    def resolve(tokn):
        """A bound is either a CFG_* macro or an integer literal written
        in place (_CLAMP(0, 1)). Both are real bounds; only the macro form
        has a name worth carrying into the table."""
        if tokn is None:
            return None, None
        if re.fullmatch(r"-?(?:0[xX][0-9a-fA-F]+|\d+)", tokn):
            return int(tokn, 0), None
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
        fld, sun = shadow.get(key, (None, False))
        rows.append({
            "key": key,
            # `ap_enable` has no K_* constant -- it is written as a bare string
            # literal at both its call sites. Emit it the same way rather than
            # inventing a constant the rest of the tree does not know about.
            "kconst": kconsts.get(key, '"%s"' % key),
            "ns": nsmap.get(key),
            "kind": kind,
            "published": key in sets["E"],
            "min": mn, "max": mx, "min_macro": mn_m, "max_macro": mx_m,
            "param": pids.get(key, "LOG_PARAM_NONE"),
            "shadow": fld,
            "sun": sun,
            "channel": (int(key[-1]) if re.match(r"(travel|dwell_open|dwell_close)_m[123]$", key)
                        else 0),
        })
    return rows, pub


NS_ORDER = ("NVS_NS_CLIMATE", "NVS_NS_WIND", "NVS_NS_MOTOR",
            "NVS_NS_WIFI", "NVS_NS_SYSTEM")

EMIT_HEADER = '''/* SPDX-License-Identifier: MIT */
/**
 * @file cfg_desc.inc
 * @brief The config key descriptor table -- ONE row per key (gh#64).
 *
 * ============================================================================
 *  GENERATED FILE -- do not edit by hand.
 *  Regenerate:  python bin/gen_cfg_desc.py --emit
 *  Verify:      python bin/gen_cfg_desc.py --check   (non-zero on drift)
 * ============================================================================
 *
 * Until 2.8.x the key set was written out six times -- cfg_key_kind(),
 * the apply_config_update() shadow ladder, cfg_clamp(), ns_key_to_log_id(),
 * LIMITS_JSON and the mock server -- and they drifted. Every drift so far
 * reached a release: gh#53 (a whole kind of key rejected), gh#57 part 1
 * (eleven keys accepting any int32), gh#57 part 2 (a clamp wider than the
 * published range), gh#51 group B (a motor key changing unaudited).
 *
 * The fix is not a seventh list. It is that there is only this one, and the
 * consumers read it. Adding a key is one row.
 *
 * Fields that are NOT here, on purpose:
 *   - The bounds themselves. Those stay in cfg_limits.h, referenced by macro,
 *     so a bound is still changed in exactly one place.
 *   - String-valued keys (tz_str, ota_url, status_secret, ...). They never
 *     travel through Q4; their endpoints validate and log them directly.
 *
 * CFG_KEY_NOT_Q4 rows are real keys with real bounds and a real audit id that
 * a DIFFERENT route owns -- the four ota_* belong to POST /api/ota/config.
 * They are listed so their bounds have one home, and cfg_key_kind() maps them
 * to CFG_KEY_UNKNOWN so Q4 still refuses them exactly as before.
 */

/* offsetof and sizeof are taken from the SAME field expression, so the two
 * cannot disagree with each other, nor with the name written beside the key. */
#define CFG_SH(f)   (uint16_t)offsetof(cfg_shadow_t, f), \\
                    (uint8_t)sizeof(((cfg_shadow_t *)0)->f)
#define CFG_NOSH    0u, 0u

static const cfg_desc_t CFG_DESC[] = {
'''


def emit_c(rows):
    """The descriptor table as C, ordered by namespace then key."""
    out = [EMIT_HEADER]
    by_ns = {}
    for r in rows:
        by_ns.setdefault(r["ns"], []).append(r)

    def bound(val, macro):
        return macro if macro else str(val)

    for nsc in NS_ORDER:
        group = sorted(by_ns.get(nsc, []), key=lambda r: r["key"])
        if not group:
            continue
        out.append("    /* ---- %s ---- */\n" % nsc)
        for r in group:
            flags = []
            if r["published"]:
                flags.append("CFG_F_PUB")
            if r["sun"]:
                flags.append("CFG_F_SUN")
            out.append(
                "    { %s, %s, %s, %uu, %s, %s,\n"
                "      %s, %s, %s },\n"
                % (nsc, r["kconst"], r["kind"].replace("CFG_KIND_", "CFG_KEY_"),
                   r["channel"], (" | ".join(flags) or "0u"), r["param"],
                   bound(r["min"], r["min_macro"]), bound(r["max"], r["max_macro"]),
                   ("CFG_SH(%s)" % r["shadow"]) if r["shadow"] else "CFG_NOSH"))
        out.append("\n")
    out.append("};\n\n#undef CFG_SH\n#undef CFG_NOSH\n")
    return "".join(out)


DESC_PATH = os.path.join(ROOT, "firmware", "config", "cfg_desc.inc")


def emit_main(rows, write):
    """Write (or verify) firmware/config/cfg_desc.inc.

    `--check` is what the pre-commit hook runs: it re-derives the table from the
    sources and fails if the committed file has drifted from them. That is the
    whole safety property -- the generated file is only trustworthy if something
    proves, every commit, that it still matches what the code actually does.
    """
    text = emit_c(rows)
    have = read(DESC_PATH) if os.path.exists(DESC_PATH) else None
    if write:
        with open(DESC_PATH, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        print("%s  (%d rows, %s)"
              % (os.path.relpath(DESC_PATH, ROOT), len(rows),
                 "unchanged" if have == text else "UPDATED"))
        return 0
    if have is None:
        print("MISSING: %s -- run python bin/gen_cfg_desc.py --emit"
              % os.path.relpath(DESC_PATH, ROOT))
        return 1
    if have != text:
        import difflib
        print("DRIFT: %s no longer matches the tables it was derived from."
              % os.path.relpath(DESC_PATH, ROOT))
        for line in list(difflib.unified_diff(
                have.splitlines(), text.splitlines(),
                "committed", "derived", lineterm=""))[:40]:
            print("  %s" % line)
        print("\nRegenerate with: python bin/gen_cfg_desc.py --emit")
        return 1
    print("%s matches the tables it was derived from (%d rows)"
          % (os.path.relpath(DESC_PATH, ROOT), len(rows)))
    return 0


def verify(rows, pub, quiet):
    """The four self-checks. A derived table is only worth having if something
    proves the derivation still describes what the code does, so these run on
    EVERY path -- printing, emitting and checking alike. Returns 0 or 1."""
    def say(msg):
        if not quiet:
            print(msg)

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
    # Check 3 -- every key must carry bounds. A key the write path does not
    # range-check is gh#57 part 1 exactly: POST /api/config stored any int32
    # for eleven of them. This check is what caught status_expose being read as
    # unbounded, because its _CLAMP(0, 0x3F) is written in hex and the bound
    # token regex accepted only decimal.
    unbounded = [r["key"] for r in rows if r["min"] is None]
    # Check 4 -- being SHADOW and having a shadow field must be the same thing.
    # cfg_key_kind() asserts that equivalence in prose ("a key is SHADOW exactly
    # when that ladder has an arm writing a cfg_shadow_t field for it"); here it
    # is tested. 2.4.6 shipped a release in which it was false.
    miskind = [r["key"] for r in rows
               if (r["kind"] == "CFG_KIND_SHADOW") != (r["shadow"] is not None)]
    nons = [r["key"] for r in rows if not r["ns"]]

    say("\n%d keys" % len(rows))
    if bad:
        print("MISMATCH between cfg_clamp() and LIMITS_JSON for: %s" % ", ".join(bad))
        return 1
    say("clamped bounds agree with published bounds for every published key")
    if unbounded:
        print("UNBOUNDED (the write path range-checks nothing): %s" % ", ".join(unbounded))
        return 1
    say("every key carries a clamp range")
    if nons:
        print("NO NAMESPACE resolved for: %s" % ", ".join(nons))
        return 1
    if miskind:
        print("KIND/SHADOW DISAGREE for: %s" % ", ".join(miskind))
        return 1
    say("every SHADOW key has a shadow field, and no other key has one")
    if problems:
        for m in problems:
            print("  DISAGREES  %s" % m)
        return 1
    say("every key's audit and publish status matches its declared gap")
    return 0


def use_revision(rev):
    """Point every path at the sources as they were at git revision @p rev.

    The six tables this script derives from NO LONGER EXIST on main -- collapsing
    them is what gh#64 did, and this script is what produced the table that
    replaced them. Without this flag it would be a script in bin/ that fails
    when run, which in this repo is a documented way to lose an afternoon.

    With it the derivation stays reproducible from history, so the claim "the
    descriptor reproduces what the six tables did" remains something anyone can
    re-run rather than something they have to take on trust:

        python bin/gen_cfg_desc.py --from-rev ':/tools.gh#64' --check
    """
    import subprocess, tempfile
    global DM_PATH, WS_PATH, LIM_PATH

    # Resolve to a SHA before doing anything else. `git show <rev>:<path>` cannot
    # parse a `:/text` search spec glued to a path, and `:/text` is exactly what
    # this is normally called with -- the commit is cherry-picked between main
    # and ropeSensor, so its hash is not stable and must not be pinned.
    try:
        sha = subprocess.check_output(["git", "rev-parse", "--verify", rev],
                                      cwd=ROOT, stderr=subprocess.STDOUT)
        sha = sha.decode().strip()
    except subprocess.CalledProcessError as exc:                  # noqa: BLE001
        sys.exit("cannot resolve revision %r: %s"
                 % (rev, exc.output.decode(errors="replace").strip()))

    # `rev` may contain path-hostile characters; name the temp dir by the SHA.
    tmp = tempfile.mkdtemp(prefix="cfgdesc-%s-" % sha[:12])
    wanted = {
        "firmware/src/data_manager/data_manager.cpp": None,
        "firmware/src/web_server/web_server.cpp": None,
        "firmware/config/cfg_limits.h": None,
        "webUiMock/mock_server.py": None,
    }
    for rel in list(wanted):
        dst = os.path.join(tmp, *rel.split("/"))
        d = os.path.dirname(dst)
        if not os.path.isdir(d):
            os.makedirs(d)
        try:
            blob = subprocess.check_output(["git", "show", "%s:%s" % (sha, rel)],
                                           cwd=ROOT)
        except subprocess.CalledProcessError:
            sys.exit("cannot read %s at revision %s" % (rel, sha))
        with open(dst, "wb") as fh:
            fh.write(blob)
        wanted[rel] = dst

    DM_PATH = wanted["firmware/src/data_manager/data_manager.cpp"]
    WS_PATH = wanted["firmware/src/web_server/web_server.cpp"]
    LIM_PATH = wanted["firmware/config/cfg_limits.h"]
    # check_cfg_tables.py resolves its four sources relative to the working
    # directory, so putting us inside the extracted tree redirects it too.
    os.chdir(tmp)
    print("reading the pre-refactor tables from %s (%s)\n" % (rev, sha[:9]))


def main():
    rev = None
    for i, a in enumerate(sys.argv):
        if a == "--from-rev" and i + 1 < len(sys.argv):
            rev = sys.argv[i + 1]
        elif a.startswith("--from-rev="):
            rev = a.split("=", 1)[1]
    if rev:
        use_revision(rev)

    rows, pub = build()
    emit = "--emit" in sys.argv
    check = "--check" in sys.argv

    if not (emit or check):
        print("%-16s %-8s %-9s %-3s %-22s %-11s %s"
              % ("key", "ns", "kind", "pub", "param_id", "bounds", "shadow field"))
        print("-" * 100)
        for r in rows:
            b = ("%s..%s" % (r["min"], r["max"])) if r["min"] is not None else "(none)"
            print("%-16s %-8s %-9s %-3s %-22s %-11s %s%s"
                  % (r["key"], (r["ns"] or "?").replace("NVS_NS_", "").lower(),
                     r["kind"].replace("CFG_KIND_", ""),
                     "y" if r["published"] else ".", r["param"], b,
                     r["shadow"] or "-", "  +sun" if r["sun"] else ""))

    # Never emit a table the derivation itself cannot vouch for: a wrong
    # descriptor is worse than the six-table drift it replaces, because the
    # consumers would agree with each other while all being wrong together.
    rc = verify(rows, pub, quiet=(emit or check))
    if rc:
        return rc
    if emit:
        return emit_main(rows, True)
    if check:
        return emit_main(rows, False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
