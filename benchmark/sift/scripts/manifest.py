#!/usr/bin/env python3
"""
manifest.py -- the build matrix manifest: written by build-matrix.sh, read by
run-headlines.sh. Not meant to be run by hand, except `configs`.

    manifest.py entry                     one configuration, from BM_* variables
    manifest.py assemble BUILDS           BUILDS/manifest.json from every entry
    manifest.py configs MANIFEST          one tab-separated line per configuration:
                                          name status variant backend knob
                                          sylvan_bench cudd_bench detail
    manifest.py runtime-flag MANIFEST N   the sylvan_bench flag of runtime knob N

`entry` writes BM_DIR/build-matrix.json and, for a configuration that built,
a BINARY.row.json next to sylvan_bench and cudd_bench: the members lib.sh adds
to every row those binaries produce.
"""

import glob
import json
import os
import re
import subprocess
import sys
import time

SCHEMA = "sift-build-matrix/1"


def env(name, default=""):
    return os.environ.get(name, default)


def lines(name):
    return [s for s in env(name).splitlines() if s.strip()]


def table(text, fields):
    out = []
    for line in text.splitlines():
        if not line.strip():
            continue
        cells = [c.strip() for c in line.split("|")]
        out.append(dict(zip(fields, cells)))
    return out


def option_table():
    rows = table(env("BM_MATRIX_TABLE"),
                 ["variant", "definitions", "knob", "backends", "headlines", "what"])
    for r in rows:
        r["definitions"] = r["definitions"].split()
        r["backends"] = r["backends"].split()
        r["headlines"] = r["headlines"].split()
        r["knob"] = r["knob"] or None
    return rows


def runtime_table():
    rows = table(env("BM_RUNTIME_TABLE"), ["name", "flag", "headlines", "what"])
    for r in rows:
        r["headlines"] = r["headlines"].split()
    return rows


def read_cache(path):
    """{NAME: (TYPE, VALUE)} from a CMakeCache.txt."""
    cache = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r"^([A-Za-z_][A-Za-z0-9_.-]*):([A-Z]+)=(.*)$", line.rstrip("\r\n"))
                if m:
                    cache[m.group(1)] = (m.group(2), m.group(3))
    except OSError:
        pass
    return cache


def run(cmd, cwd=None):
    try:
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                              timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def git_rev(path):
    if not path or not os.path.isdir(path):
        return None
    top = run(["git", "-C", path, "rev-parse", "--show-toplevel"]).strip()
    if not top or os.path.realpath(top) != os.path.realpath(path):
        return None
    return run(["git", "-C", path, "rev-parse", "HEAD"]).strip() or None


def lace_info(build_dir, cache):
    src = cache.get("FETCHCONTENT_SOURCE_DIR_LACE", (None, ""))[1] or \
        os.path.join(build_dir, "_deps", "lace-src")
    info = {"source": src if os.path.isdir(src) else None,
            "how": "FETCHCONTENT_SOURCE_DIR_LACE" if "FETCHCONTENT_SOURCE_DIR_LACE" in cache
            and cache["FETCHCONTENT_SOURCE_DIR_LACE"][1] else "FetchContent (GIT_TAG v2.4.0)",
            "version": None, "git": None}
    try:
        with open(os.path.join(src, "CMakeLists.txt"), encoding="utf-8") as f:
            m = re.search(r"project\s*\(\s*lace[^)]*VERSION\s+([0-9.]+)", f.read(), re.S | re.I)
            if m:
                info["version"] = m.group(1)
    except OSError:
        pass
    info["git"] = git_rev(src)
    return info


def cudd_info():
    d = env("BM_CUDD_DIR")
    if not d:
        return None
    info = {"dir": d, "git": env("BM_CUDD_GIT") or None, "version": None, "cflags": None}
    try:
        with open(os.path.join(d, "config.h"), encoding="utf-8", errors="replace") as f:
            m = re.search(r'#define\s+PACKAGE_VERSION\s+"([^"]*)"', f.read())
            if m:
                info["version"] = m.group(1)
    except OSError:
        pass
    try:
        with open(os.path.join(d, "Makefile"), encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("CFLAGS ="):
                    info["cflags"] = line.split("=", 1)[1].strip()
                    break
    except OSError:
        pass
    return info


def help_flags(binary):
    if not binary:
        return []
    return sorted(set(re.findall(r"(?<![\w-])--[a-z][a-z0-9-]*", run([binary, "--help"]))))


def write_json(path, obj, compact=False):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        if compact:
            f.write(json.dumps(obj, separators=(",", ":"), sort_keys=False))
        else:
            json.dump(obj, f, indent=1)
        f.write("\n")
    os.replace(tmp, path)


def entry():
    d = env("BM_DIR")
    cache = read_cache(os.path.join(d, "CMakeCache.txt"))
    knob_names = env("BM_ALL_KNOBS").split()
    record = env("BM_RECORD_VARS").split()
    knobs = {}
    for k in record + [k for k in knob_names if k not in record]:
        t, v = cache.get(k, (None, None))
        knobs[k] = None if t in (None, "UNINITIALIZED") else v
    status = env("BM_STATUS")
    sb = os.path.join(d, "benchmark", "sift", "sylvan_bench")
    cb = os.path.join(d, "benchmark", "sift", "cudd_bench")
    sb = sb if status == "ok" and os.access(sb, os.X_OK) else None
    cb = cb if status == "ok" and os.access(cb, os.X_OK) else None
    knob = env("BM_KNOB") or None
    e = {
        "name": env("BM_NAME"),
        "backend": env("BM_BACKEND"),
        "variant": env("BM_VARIANT"),
        "build_type": "Release",
        "knob": knob,
        "knob_setting": knobs.get(knob) if knob else None,
        "definitions": env("BM_DEFS").split(),
        "headlines": env("BM_HEADLINES").split(),
        "what": env("BM_WHAT"),
        "status": status,
        "status_detail": env("BM_DETAIL") or None,
        "dir": d,
        "cmake_args": lines("BM_ARGS"),
        "unused_vars": lines("BM_UNUSED"),
        "knobs": knobs,
        "sylvan_bench": sb,
        "sylvan_bench_flags": help_flags(sb),
        "cudd_bench": cb,
        "src": env("BM_SRC"),
        "src_git": env("BM_SRC_GIT"),
        "src_dirty": None if env("BM_SRC_DIRTY") in ("", "null") else int(env("BM_SRC_DIRTY")),
        "src_rev_from": env("BM_SRC_REV_FROM"),
        "cc": env("BM_CC"),
        "cc_path": env("BM_CC_PATH"),
        "cc_version": env("BM_CC_VERSION"),
        "lace": lace_info(d, cache),
        "cudd": cudd_info() if cb else None,
        "configured_at": env("BM_CONFIGURED_AT"),
        "built_at": env("BM_BUILT_AT") or None,
        "seconds": int(env("BM_SECONDS") or 0),
        "configure_log": os.path.join(d, "configure.log"),
        "build_log": os.path.join(d, "build.log") if os.path.exists(os.path.join(d, "build.log")) else None,
    }
    write_json(os.path.join(d, "build-matrix.json"), e)
    if sb:
        write_json(sb + ".row.json", {
            "config_name": e["name"],
            "config_variant": e["variant"],
            "config_status": e["status"],
            "config_knob": knob,
            "knobs": knobs,
            "cc_version": e["cc_version"],
            "src_git": e["src_git"],
            "lace_version": e["lace"]["version"],
        }, compact=True)
    if cb:
        c = e["cudd"] or {}
        write_json(cb + ".row.json", {
            "config_name": "cudd",
            "config_variant": "cudd",
            "config_status": "ok",
            "cudd_version": c.get("version"),
            "cudd_git": c.get("git"),
            "cudd_cflags": c.get("cflags"),
            "cc_version": e["cc_version"],
        }, compact=True)


def assemble(builds):
    entries = []
    for p in sorted(glob.glob(os.path.join(builds, "*", "build-matrix.json"))):
        try:
            with open(p, encoding="utf-8") as f:
                entries.append(json.load(f))
        except (OSError, json.JSONDecodeError) as ex:
            print(f"manifest.py: skipping {p}: {ex}", file=sys.stderr)
    order = {}
    for i, r in enumerate(option_table()):
        for j, b in enumerate(r["backends"]):
            order[(r["variant"], b)] = (i, j)
    entries.sort(key=lambda e: (order.get((e["variant"], e["backend"]), (99, 99)), e["name"]))
    srcs = sorted({e.get("src_git") for e in entries})
    ccs = sorted({e.get("cc_version") for e in entries})
    m = {
        "schema": SCHEMA,
        "written_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "src": env("BM_SRC"),
        "src_git": env("BM_SRC_GIT"),
        "src_dirty": None if env("BM_SRC_DIRTY") in ("", "null") else int(env("BM_SRC_DIRTY")),
        "src_rev_from": env("BM_SRC_REV_FROM"),
        "mixed_sources": len(srcs) > 1,
        "cc": env("BM_CC"),
        "cc_version": env("BM_CC_VERSION"),
        "mixed_compilers": len(ccs) > 1,
        "cmake_version": run(["cmake", "--version"]).splitlines()[0:1] or None,
        "ninja_version": run(["ninja", "--version"]).strip() or None,
        "generator": "Ninja",
        "host": {"uname": " ".join(os.uname()) if hasattr(os, "uname") else None,
                 "nproc": os.cpu_count()},
        "cudd": cudd_info(),
        "option_table": option_table(),
        "runtime_knobs": runtime_table(),
        "builds": {e["name"]: e for e in entries},
    }
    if m["cmake_version"]:
        m["cmake_version"] = m["cmake_version"][0]
    write_json(os.path.join(builds, "manifest.json"), m)
    if m["mixed_sources"]:
        print(f"manifest.py: WARNING: the builds come from different commits: {srcs}",
              file=sys.stderr)
    if m["mixed_compilers"]:
        print(f"manifest.py: WARNING: the builds use different compilers: {ccs}",
              file=sys.stderr)


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def configs(path):
    m = load(path)
    for e in m["builds"].values():
        print("\t".join(str(x) if x is not None else "-" for x in (
            e["name"], e["status"], e["variant"], e["backend"], e.get("knob"),
            e.get("sylvan_bench"), e.get("cudd_bench"),
            (e.get("status_detail") or "-").replace("\t", " "))))


def runtime_flag(path, name):
    for r in load(path).get("runtime_knobs", []):
        if r["name"] == name:
            print(r["flag"])
            return
    sys.exit(f"manifest.py: no runtime knob '{name}' in {path}")


def main():
    a = sys.argv[1:]
    if a[:1] == ["entry"]:
        entry()
    elif a[:1] == ["assemble"] and len(a) == 2:
        assemble(a[1])
    elif a[:1] == ["configs"] and len(a) == 2:
        configs(a[1])
    elif a[:1] == ["runtime-flag"] and len(a) == 3:
        runtime_flag(a[1], a[2])
    else:
        sys.exit(__doc__.strip())


if __name__ == "__main__":
    main()
