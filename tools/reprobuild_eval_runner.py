#!/usr/bin/env python3
import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shlex
import signal
import subprocess
import time
from pathlib import Path

try:
    import yaml
except Exception:
    yaml = None


PROJECTS = [
    ("tiny-AES-c", "make", "make clean", 1200, None),
    ("zlib", "make", "make clean", 1200, None),
    ("lz4", "make -j2", "make clean", 1800, None),
    ("lua", "make -j2", "make clean", 1800, None),
    ("libtommath", "make -j2", "make clean", 1800, None),
    ("busybox", "make -j2", "make clean", 3600, None),
    ("e2fsprogs", "cd build && make -j2", "cd build && make clean", 3600, ["bash", "-lc", "cd build && make -j2"]),
    ("openssl", "bash -lc './Configure && make -j8 build_sw'", "make clean", 5400, ["bash", "-lc", "./Configure && make -j8 build_sw"]),
    ("git", "bash -lc 'LC_ALL=C GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0=\"$PWD\" make -j2 prefix=/home/sprooc NO_GETTEXT=YesPlease NO_TCLTK=YesPlease NO_CURL=YesPlease NO_EXPAT=YesPlease all'", "make clean", 5400, ["bash", "-lc", "LC_ALL=C GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0=\"$PWD\" make -j2 prefix=/home/sprooc NO_GETTEXT=YesPlease NO_TCLTK=YesPlease NO_CURL=YesPlease NO_EXPAT=YesPlease all"]),
    ("ffmpeg", "bash -lc './configure --disable-doc --disable-x86asm && make -j2 REVISION=8.0.git'", "if [ -f Makefile ]; then make distclean || make clean; fi", 7200, ["bash", "-lc", "./configure --disable-doc --disable-x86asm && make -j2 REVISION=8.0.git"]),
]

EXCLUDED_PROJECTS = {"poco"}


def now():
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def mkdir(path):
    Path(path).mkdir(parents=True, exist_ok=True)


def write(path, text):
    mkdir(Path(path).parent)
    Path(path).write_text(text, encoding="utf-8")


def append(path, text):
    mkdir(Path(path).parent)
    with Path(path).open("a", encoding="utf-8") as f:
        f.write(text)


def run_capture(argv, cwd, timeout=60):
    try:
        cp = subprocess.run(
            argv,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
        return cp.returncode, cp.stdout
    except Exception as exc:
        return 999, repr(exc)


def run_logged(argv, cwd, log_path, timeout=None):
    start = time.time()
    mkdir(Path(log_path).parent)
    with Path(log_path).open("w", encoding="utf-8", errors="replace") as f:
        f.write(f"start_utc={now()}\n")
        f.write(f"cwd={cwd}\n")
        f.write("argv=" + json.dumps(argv, ensure_ascii=False) + "\n")
        f.write(f"timeout={timeout}\n\n")
        f.flush()
        proc = subprocess.Popen(
            argv,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            preexec_fn=os.setsid,
        )
        timed_out = False
        try:
            for line in proc.stdout:
                f.write(line)
                f.flush()
            rc = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                rc = proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                rc = proc.wait()
        elapsed = time.time() - start
        f.write(f"\nend_utc={now()}\nreturncode={rc}\ntimed_out={int(timed_out)}\nrunner_elapsed={elapsed:.3f}\n")
    return {"returncode": rc, "timed_out": timed_out, "elapsed": elapsed, "log": str(log_path)}


def fix_permissions(project_dir, log_path):
    uid = getattr(os, "getuid", lambda: 1000)()
    gid = getattr(os, "getgid", lambda: 1000)()
    cmd = f"if command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then sudo chown -R {uid}:{gid} .; fi"
    return run_logged(["bash", "-lc", cmd], project_dir, log_path, timeout=900)


def load_yaml(path):
    if yaml is None or not Path(path).exists():
        return None, "yaml module unavailable or file missing"
    try:
        with Path(path).open(encoding="utf-8") as f:
            return yaml.safe_load(f) or {}, ""
    except Exception as exc:
        return None, repr(exc)


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def file_type(path):
    rc, out = run_capture(["file", "-b", str(path)], Path("/"), timeout=20)
    return out.strip() if rc == 0 else ""


def parse_elapsed(log_path):
    if not Path(log_path).exists():
        return None
    text = Path(log_path).read_text(encoding="utf-8", errors="replace")
    vals = re.findall(r"elapsed=([0-9]+(?:\.[0-9]+)?)", text)
    return float(vals[-1]) if vals else None


def parse_reprobuild_timings(*log_paths):
    patterns = {
        "preprocess_ms": r"(?:Reprobuild\s+)?Preprocessing time:\s+([0-9]+)\s+ms",
        "build_ms": r"Build execution time:\s+([0-9]+)\s+ms",
        "postprocess_ms": r"(?:Reprobuild\s+)?Postprocessing time:\s+([0-9]+)\s+ms",
        "total_ms": r"Total tracking time:\s+([0-9]+)\s+ms",
    }
    result = {key: None for key in patterns}
    result["source_log"] = ""
    for log_path in log_paths:
        path = Path(log_path)
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        matched = False
        for key, pattern in patterns.items():
            vals = re.findall(pattern, text)
            if vals:
                result[key] = int(vals[-1])
                matched = True
        if matched:
            result["source_log"] = str(path)
            break
    return result


def parse_start_epoch(log_path):
    if not Path(log_path).exists():
        return None
    text = Path(log_path).read_text(encoding="utf-8", errors="replace")
    match = re.search(r"^start_utc=(\S+)", text, re.MULTILINE)
    if not match:
        return None
    try:
        return dt.datetime.fromisoformat(match.group(1).replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def project_argv(build, override):
    return override if override else shlex.split(build)


def rel(path, root):
    try:
        return str(Path(path).resolve().relative_to(Path(root).resolve()))
    except Exception:
        return str(path)


def git_state(project_dir):
    state = {}
    for key, argv in {
        "head": ["git", "-C", str(project_dir), "rev-parse", "HEAD"],
        "remote": ["git", "-C", str(project_dir), "remote", "-v"],
        "status_short": ["git", "-C", str(project_dir), "status", "--short"],
    }.items():
        rc, out = run_capture(argv, project_dir, timeout=60)
        state[key] = out.strip() if rc == 0 else ""
    return state


def capture_environment(root, c_build_root, run_dir):
    env_dir = run_dir / "environment"
    mkdir(env_dir)
    cmds = {
        "date_utc": ["date", "-u", "+%Y-%m-%dT%H:%M:%SZ"],
        "hostname": ["hostname"],
        "uname": ["uname", "-a"],
        "os_release": ["bash", "-lc", "cat /etc/os-release"],
        "lscpu": ["lscpu"],
        "memory": ["free", "-h"],
        "docker_version": ["docker", "--version"],
        "docker_info": ["bash", "-lc", "docker info 2>&1 | sed -n '1,140p'"],
        "go_version": ["go", "version"],
        "bpftrace_version": ["sudo", "-n", "/usr/bin/bpftrace0.24", "--version"],
        "reprobuild_commit": ["git", "-C", str(root), "rev-parse", "HEAD"],
        "reprobuild_status": ["git", "-C", str(root), "status", "--short"],
        "c_build_commit": ["git", "-C", str(c_build_root), "rev-parse", "HEAD"],
        "c_build_status": ["git", "-C", str(c_build_root), "status", "--short"],
        "reprobuild_binary": ["bash", "-lc", f"ls -l {shlex.quote(str(root / 'build/reprobuild'))}; sha256sum {shlex.quote(str(root / 'build/reprobuild'))}"],
    }
    merged = []
    for name, argv in cmds.items():
        rc, out = run_capture(argv, root, timeout=120)
        write(env_dir / f"{name}.log", f"argv={json.dumps(argv)}\nreturncode={rc}\n\n{out}")
        merged.append(f"## {name}\nreturncode={rc}\n\n{out.strip()}\n")
    write(env_dir / "environment_summary.md", "\n".join(merged))


def dependency_counts(deps):
    counts = {"total": len(deps), "tool": 0, "library": 0, "header": 0, "custom": 0, "other": 0}
    names = set()
    for dep in deps:
        path = str(dep.get("path", ""))
        name = str(dep.get("name", ""))
        origin = str(dep.get("origin", ""))
        if name:
            names.add(name)
        if origin == "custom" or name == "custom":
            counts["custom"] += 1
        elif path.startswith("/usr/bin/") or path.startswith("/bin/") or "/bin/" in path:
            counts["tool"] += 1
        elif path.endswith((".h", ".hpp", ".hh")) or "/include/" in path:
            counts["header"] += 1
        elif ".so" in path or path.endswith((".a", ".la")) or "/lib/" in path or "/lib64/" in path:
            counts["library"] += 1
        else:
            counts["other"] += 1
    counts["unique_names"] = len(names)
    return counts, names


def manual_artifacts(project_dir, limit=80, min_mtime=None):
    skip = {".git", ".repro_experiments", "tracker_logs", "logs"}
    rows = []
    for root, dirs, files in os.walk(project_dir):
        dirs[:] = [d for d in dirs if d not in skip]
        for name in files:
            p = Path(root) / name
            try:
                stat = p.stat()
                if min_mtime is not None and stat.st_mtime < min_mtime:
                    continue
                suffixes = p.suffixes
                has_shared_lib_suffix = ".so" in suffixes
                has_static_lib_suffix = p.suffix == ".a"
                if not (os.access(p, os.X_OK) or has_shared_lib_suffix or has_static_lib_suffix):
                    continue
                ft = file_type(p)
                if "ELF" in ft or "current ar archive" in ft or has_shared_lib_suffix or has_static_lib_suffix:
                    rows.append({"path": rel(p, project_dir), "size": stat.st_size, "type": ft})
            except Exception:
                pass
    rows.sort(key=lambda x: (-x["size"], x["path"]))
    return rows[:limit], len(rows)


def ldd_coverage(project_dir, artifacts, dep_names, log_dir):
    mkdir(log_dir)
    checked = []
    covered = 0
    total = 0
    for art in artifacts[:5]:
        raw = str(art.get("path", ""))
        p = Path(raw)
        if not p.is_absolute():
            p = project_dir / raw
        if not p.exists() or "ELF" not in file_type(p):
            continue
        safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", rel(p, project_dir))
        rc, out = run_capture(["ldd", str(p)], project_dir, timeout=60)
        write(log_dir / f"ldd_{safe}.log", f"returncode={rc}\nartifact={p}\n\n{out}")
        libs = []
        for line in out.splitlines():
            m = re.search(r"=>\s+(/\S+)", line) or re.search(r"^\s*(/\S+)", line)
            if m:
                libs.append(m.group(1))
        details = []
        for lib in libs:
            pkg, real_path = dpkg_package_for_file(lib, project_dir)
            hit = bool(pkg and pkg in dep_names)
            total += 1
            covered += 1 if hit else 0
            details.append({"path": lib, "real_path": real_path, "package": pkg, "covered": hit})
        checked.append({"artifact": rel(p, project_dir), "libs": details})
    return {"checked": checked, "covered": covered, "total": total, "summary": f"{covered}/{total}" if total else "no dynamic ELF artifact checked"}


def dpkg_package_for_file(path, cwd):
    # Match DependencyPackage::fromRawFile(): try the raw path, then the
    # canonical path. This matters on usrmerge systems where /lib paths are
    # owned in dpkg's database under /usr/lib.
    real_path = os.path.realpath(path)
    for candidate in [path, real_path]:
        if not candidate:
            continue
        rc, out = run_capture(["dpkg", "-S", candidate], cwd, timeout=30)
        if rc != 0:
            continue
        for line in out.splitlines():
            if not line or line.startswith("diversion by"):
                continue
            if ":" in line:
                return line.split(":", 1)[0].strip(), real_path
    return "", real_path


def graph_counts(path):
    data, err = load_yaml(path)
    if data is None:
        return {"parse_ok": False, "error": err, "nodes": 0, "edges": 0, "artifact_nodes": 0}
    nodes = data.get("nodes") or []
    edges = data.get("edges") or []
    artifact_nodes = sum(1 for n in nodes if str(n.get("type", "")).lower() == "artifact")
    return {"parse_ok": True, "error": "", "nodes": len(nodes), "edges": len(edges), "artifact_nodes": artifact_nodes}


def analyze_project(root, run_dir, item):
    name, build, clean, timeout, argv_override = item
    project_dir = root / "projs" / name
    pdir = run_dir / name
    outer_record = pdir / "host" / "build_record.yaml"
    rebuild_record = pdir / "rebuild" / "host" / "build_record.yaml"
    record = rebuild_record if rebuild_record.exists() else outer_record
    record_source = "rebuild/host" if record == rebuild_record else "host"
    outer_graph = pdir / "host" / "build_graph.yaml"
    rebuild_graph = pdir / "rebuild" / "host" / "build_graph.yaml"
    graph = rebuild_graph if rebuild_graph.exists() else outer_graph
    graph_source = "rebuild/host" if graph == rebuild_graph else "host"
    data, err = load_yaml(record)
    metadata = data.get("metadata", {}) if isinstance(data, dict) else {}
    deps = (data.get("dependencies") or []) if isinstance(data, dict) else []
    artifacts = (data.get("artifacts") or []) if isinstance(data, dict) else []
    commits = (data.get("git_commit_ids") or []) if isinstance(data, dict) else []
    dep_counts, dep_names = dependency_counts(deps)

    checks = []
    hash_ok = 0
    exists = 0
    false_reports = []
    for art in artifacts:
        raw = str(art.get("path", ""))
        p = Path(raw)
        if not p.is_absolute():
            p = project_dir / raw
        row = {"record_path": raw, "resolved_path": str(p), "exists": p.exists(), "record_hash": art.get("hash", ""), "actual_hash": "", "hash_match": False, "file_type": ""}
        if p.exists() and p.is_file():
            exists += 1
            try:
                row["actual_hash"] = sha256(p)
                row["hash_match"] = row["actual_hash"] == row["record_hash"]
                hash_ok += 1 if row["hash_match"] else 0
                row["file_type"] = file_type(p)
            except Exception as exc:
                row["error"] = repr(exc)
        else:
            false_reports.append(raw)
        checks.append(row)

    manual_since = parse_start_epoch(pdir / "00_raw_time.log")
    manual, manual_total = manual_artifacts(project_dir, min_mtime=manual_since)
    rec_paths = {str(a.get("path", "")) for a in artifacts}
    rec_names = {Path(str(a.get("path", ""))).name for a in artifacts}
    missed = [m for m in manual if m["path"] not in rec_paths and Path(m["path"]).name not in rec_names]

    raw_t = parse_elapsed(pdir / "00_raw_time.log")
    timing = parse_reprobuild_timings(
        pdir / "rebuild" / "logs" / "01_reprobuild_track.log",
        pdir / "02_container_rebuild.log",
        pdir / "01_tracked_time.log",
    )
    tracked_total_t = (timing["total_ms"] / 1000.0) if timing["total_ms"] is not None else parse_elapsed(pdir / "01_tracked_time.log")
    tracked_build_t = (timing["build_ms"] / 1000.0) if timing["build_ms"] is not None else None
    tracked_preprocess_t = (timing["preprocess_ms"] / 1000.0) if timing["preprocess_ms"] is not None else None
    tracked_postprocess_t = (timing["postprocess_ms"] / 1000.0) if timing["postprocess_ms"] is not None else None
    overhead = ((tracked_total_t - raw_t) / raw_t * 100.0) if raw_t and tracked_total_t else None
    log_size = 0
    tracker_dirs = [pdir / "rebuild" / "tracker_logs", pdir / "tracker_logs"]
    for tracker_dir in tracker_dirs:
        if not tracker_dir.exists():
            continue
        for pat in ["bpftrace_raw_output_*.log", "bpftrace_*.log"]:
            files = list(tracker_dir.glob(pat))
            if files:
                log_size = sum(f.stat().st_size for f in files)
                break
        if log_size:
            break

    rebuild_summary = {}
    summary_txt = pdir / "rebuild" / "summary.txt"
    if summary_txt.exists():
        for line in summary_txt.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                rebuild_summary[k] = v

    return {
        "name": name,
        "project_dir": str(project_dir),
        "commands": {"build": build, "clean": clean},
        "git": git_state(project_dir),
        "record": {
            "path": str(record),
            "source": record_source,
            "outer_path": str(outer_record),
            "rebuild_host_path": str(rebuild_record),
            "exists": record.exists(),
            "parse_ok": data is not None,
            "parse_error": err,
            "metadata_present": bool(metadata),
            "metadata_keys": sorted(metadata.keys()) if isinstance(metadata, dict) else [],
            "dependency_count": len(deps),
            "artifact_count": len(artifacts),
            "git_commit_count": len(commits),
            "record_size": record.stat().st_size if record.exists() else 0,
        },
        "dependencies": {"counts": dep_counts, "ldd_coverage": ldd_coverage(project_dir, artifacts, dep_names, pdir / "logs" / "dynamic_deps")},
        "artifacts": {
            "checks": checks,
            "exists_count": exists,
            "hash_ok": hash_ok,
            "false_reports": false_reports,
            "manual_total": manual_total,
            "manual_since_epoch": manual_since,
            "manual_sample": manual[:12],
            "missed_sample": missed[:12],
            "missed_count_in_sample": len(missed),
        },
        "graph": {
            **graph_counts(graph),
            "path": str(graph),
            "source": graph_source,
            "outer_path": str(outer_graph),
            "rebuild_host_path": str(rebuild_graph),
            "size": graph.stat().st_size if graph.exists() else 0,
        },
        "performance": {
            "raw_time": raw_t,
            "tracked_time": tracked_total_t,
            "tracked_total_time": tracked_total_t,
            "tracked_build_time": tracked_build_t,
            "tracked_preprocess_time": tracked_preprocess_t,
            "tracked_postprocess_time": tracked_postprocess_t,
            "timing_source_log": timing["source_log"],
            "timing_ms": {
                "preprocess": timing["preprocess_ms"],
                "build": timing["build_ms"],
                "postprocess": timing["postprocess_ms"],
                "total": timing["total_ms"],
            },
            "overhead_pct": overhead,
            "record_kib": (record.stat().st_size / 1024.0) if record.exists() else 0,
            "graph_kib": (graph.stat().st_size / 1024.0) if graph.exists() else 0,
            "bpftrace_log_mib": log_size / 1024.0 / 1024.0,
        },
        "rebuild": {
            "summary": rebuild_summary,
            "exit_status": int(rebuild_summary["exit_status"]) if rebuild_summary.get("exit_status", "").isdigit() else None,
            "summary_path": str(summary_txt) if summary_txt.exists() else "",
            "debug_log": str(pdir / "rebuild" / "logs" / "02_c_build_debug.log"),
            "render_log": str(pdir / "rebuild" / "logs" / "03_c_build_render.log"),
        },
    }


def ok_word(value):
    return "pass" if value else "fail"


def num(value, digits=2):
    return "NA" if value is None else f"{value:.{digits}f}"


def record_capture_ok(project):
    record = project.get("record", {})
    if not (record.get("exists") and record.get("parse_ok")):
        return False
    tracked_step = project.get("steps", {}).get("tracked")
    if isinstance(tracked_step, dict):
        return tracked_step.get("returncode") == 0
    return True


def issue_text(p):
    issues = []
    if not p["record"]["exists"]:
        issues.append("missing record")
    elif not p["record"]["parse_ok"]:
        issues.append("record parse failed")
    if p["record"]["artifact_count"] == 0:
        issues.append("no artifacts")
    if p["artifacts"]["missed_sample"]:
        issues.append("manual sample missed artifacts")
    if not p["graph"]["parse_ok"]:
        issues.append("graph parse failed")
    if p["rebuild"]["exit_status"] not in (None, 0):
        issues.append("container rebuild failed")
    return "; ".join(issues) if issues else "none"


def report(summary):
    projects = summary["projects"]
    lines = []
    lines += [
        "# reprobuild report",
        "",
        f"Run directory: `{summary['run_dir']}`",
        f"Started: `{summary['started_utc']}`",
        f"Finished: `{summary['finished_utc']}`",
        "",
        "## Environment",
        "",
        f"Environment logs: `{summary['run_dir']}/environment/`",
        "",
        "## Record Capture",
        "",
        "| Project | Capture | Metadata | Deps | Artifacts | Git Entries | Graph | Notes |",
        "| --- | --- | ---: | ---: | ---: | ---: | --- | --- |",
    ]
    for p in projects:
        tracked_ok = record_capture_ok(p)
        lines.append(f"| `{p['name']}` | {ok_word(tracked_ok)} | {1 if p['record']['metadata_present'] else 0} | {p['record']['dependency_count']} | {p['record']['artifact_count']} | {p['record']['git_commit_count']} | {ok_word(p['graph']['parse_ok'])} | {issue_text(p)} |")
    lines += [
        "",
        "## Dependencies",
        "",
        "| Project | Recorded Deps | Dynamic Lib Coverage | Tool Entries | Header Entries | Custom Deps | Notes |",
        "| --- | ---: | --- | ---: | ---: | ---: | --- |",
    ]
    for p in projects:
        c = p["dependencies"]["counts"]
        note = []
        if c["library"]:
            note.append("includes libraries")
        if c["header"]:
            note.append("includes headers")
        if c["custom"]:
            note.append("has custom deps")
        if not note:
            note.append("mostly package/tool entries")
        lines.append(f"| `{p['name']}` | {c['total']} | {p['dependencies']['ldd_coverage']['summary']} | {c['tool']} | {c['header']} | {c['custom']} | {'; '.join(note)} |")
    lines += [
        "",
        "## Artifacts",
        "",
        "| Project | Manual Artifacts | Recorded Artifacts | Hash Matches | Missed Sample | False Reports | Notes |",
        "| --- | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for p in projects:
        a = p["artifacts"]
        note = "all recorded hashes match" if p["record"]["artifact_count"] and a["hash_ok"] == p["record"]["artifact_count"] else ""
        if p["record"]["artifact_count"] == 0:
            note = "no recorded artifacts"
        if a["missed_sample"]:
            note = (note + "; " if note else "") + "manual sample missed artifacts"
        lines.append(f"| `{p['name']}` | {a['manual_total']} | {p['record']['artifact_count']} | {a['hash_ok']} | {a['missed_count_in_sample']} | {len(a['false_reports'])} | {note or 'none'} |")
    lines += [
        "",
        "## Build Graph",
        "",
        "| Project | Nodes | Edges | Artifact Nodes | Traceable | Graph Output | Notes |",
        "| --- | ---: | ---: | ---: | --- | --- | --- |",
    ]
    for p in projects:
        graph_ok = p["graph"]["parse_ok"] and p["graph"]["nodes"] > 0
        image = p.get("graph_image", "")
        image_text = f"`{image}`" if image else ("yaml parsed" if p["graph"]["parse_ok"] else "not generated")
        lines.append(f"| `{p['name']}` | {p['graph']['nodes']} | {p['graph']['edges']} | {p['graph']['artifact_nodes']} | {'yes' if graph_ok else 'no'} | {image_text} | {p['graph']['error'] or issue_text(p)} |")
    lines += [
        "",
        "## Rebuild",
        "",
        "| Project | Record Capture | Container Rebuild | Hash Matches | Graph Check | Notes | Evidence |",
        "| --- | --- | --- | ---: | --- | --- | --- |",
    ]
    for p in projects:
        record_ok = record_capture_ok(p)
        exit_status = p["rebuild"]["exit_status"]
        if exit_status == 0:
            stage, fail = "pass", "none"
        elif exit_status is None:
            stage, fail = "not run", "not run"
        else:
            stage, fail = f"fail(exit={exit_status})", "see rebuild logs"
        evidence = p["rebuild"]["summary_path"] or str(Path(summary["run_dir"]) / p["name"] / "rebuild")
        lines.append(f"| `{p['name']}` | {ok_word(record_ok)} | {stage} | {p['artifacts']['hash_ok']} | {ok_word(p['graph']['parse_ok'])} | {fail} | `{evidence}` |")
    lines += [
        "",
        "## Performance",
        "",
        "| Project | Raw Build/s | Preprocess/s | Build/s | Postprocess/s | Total/s | Overhead | Record KiB | Graph KiB | Log MiB | Deps | Artifacts | Notes |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for p in projects:
        perf = p["performance"]
        overhead = "NA" if perf["overhead_pct"] is None else f"{perf['overhead_pct']:.1f}%"
        note = "single run"
        if p.get("steps", {}).get("raw", {}).get("returncode") != 0:
            note += "; raw build non-zero"
        if not record_capture_ok(p):
            note += "; tracked build non-zero"
        lines.append(f"| `{p['name']}` | {num(perf['raw_time'])} | {num(perf.get('tracked_preprocess_time'))} | {num(perf.get('tracked_build_time'))} | {num(perf.get('tracked_postprocess_time'))} | {num(perf['tracked_time'])} | {overhead} | {perf['record_kib']:.1f} | {perf['graph_kib']:.1f} | {perf['bpftrace_log_mib']:.2f} | {p['record']['dependency_count']} | {p['record']['artifact_count']} | {note} |")
    lines += [
        "",
        "## Evidence",
        "",
        f"- Summary JSON: `{summary['summary_json']}`",
        f"- Environment logs: `{summary['run_dir']}/environment/`",
    ]
    for p in projects:
        base = Path(summary["run_dir"]) / p["name"]
        timing_log = p["performance"].get("timing_source_log") or str(base / "rebuild" / "logs" / "01_reprobuild_track.log")
        lines.append(f"- `{p['name']}`: raw `{base / '00_raw_time.log'}`; tracked `{timing_log}`; record `{p['record']['path']}`; graph `{p['graph']['path']}`; rebuild `{base / 'rebuild'}`")
    write(summary["report_md"], "\n".join(lines) + "\n")


def run_all(args):
    root = Path(args.root).resolve()
    c_build_root = Path(args.c_build_root).resolve()
    run_id = args.run_id or dt.datetime.now(dt.timezone.utc).strftime("eval_%Y%m%dT%H%M%SZ")
    run_dir = Path(args.result_root).resolve() / run_id
    mkdir(run_dir)
    capture_environment(root, c_build_root, run_dir)
    summary = {
        "started_utc": now(),
        "finished_utc": "",
        "root": str(root),
        "run_dir": str(run_dir),
        "summary_json": str(run_dir / "summary.json"),
        "report_md": str(run_dir / "report.md"),
        "projects": [],
    }
    selected_projects = list(PROJECTS)
    if args.only:
        wanted = {name.strip() for name in args.only.split(",") if name.strip()}
        excluded = sorted(wanted & EXCLUDED_PROJECTS)
        if excluded:
            print("warning: excluded project(s) ignored: " + ", ".join(excluded), flush=True)
        wanted -= EXCLUDED_PROJECTS
        selected_projects = [item for item in PROJECTS if item[0] in wanted]
        missing = sorted(wanted - {item[0] for item in selected_projects})
        if missing:
            raise SystemExit("unknown project(s): " + ", ".join(missing))
        if not selected_projects:
            raise SystemExit("no runnable projects selected")

    write(run_dir / "PROJECTS.json", json.dumps(selected_projects, indent=2, ensure_ascii=False))

    for item in selected_projects:
        name, build, clean, timeout, override = item
        project_dir = root / "projs" / name
        pdir = run_dir / name
        for d in ["host", "tracker_logs", "logs", "graph", "rebuild"]:
            mkdir(pdir / d)
        write(pdir / "notes.md", f"# {name}\n\nstart_utc={now()}\nproject_dir={project_dir}\nbuild={build}\nclean={clean}\n")
        write(pdir / "logs" / "project_state.json", json.dumps(git_state(project_dir), indent=2, ensure_ascii=False))
        steps = {}
        print(f"[{now()}] {name}: fix permissions before raw clean", flush=True)
        steps["fix_permissions_before_raw"] = fix_permissions(project_dir, pdir / "logs" / "00_fix_permissions_before_raw.log")
        print(f"[{now()}] {name}: clean before raw", flush=True)
        steps["clean_before_raw"] = run_logged(["bash", "-lc", clean], project_dir, pdir / "logs" / "00_clean_before_raw.log", timeout=600)
        print(f"[{now()}] {name}: raw build", flush=True)
        steps["raw"] = run_logged(["/usr/bin/time", "-f", "elapsed=%e maxrss=%M", "bash", "-lc", build], project_dir, pdir / "00_raw_time.log", timeout=timeout)
        print(f"[{now()}] {name}: fix permissions before container rebuild", flush=True)
        steps["fix_permissions_before_rebuild"] = fix_permissions(project_dir, pdir / "logs" / "02_fix_permissions_before_rebuild.log")
        print(f"[{now()}] {name}: container rebuild", flush=True)
        rebuild = [str(root / "tools" / "run_container_repro_experiment.sh"), f"--project={project_dir}", f"--result-dir={pdir / 'rebuild'}", f"--reprobuild-root={root}", f"--c-build-root={c_build_root}", f"--clean-cmd={clean}", "--"] + project_argv(build, override)
        steps["rebuild"] = run_logged(rebuild, root, pdir / "02_container_rebuild.log", timeout=timeout + 2400)
        analyzed = analyze_project(root, run_dir, item)
        analyzed["steps"] = steps
        append(pdir / "notes.md", f"\nend_utc={now()}\nrebuild_returncode={steps['rebuild']['returncode']}\n")
        write(pdir / "analysis.json", json.dumps(analyzed, indent=2, ensure_ascii=False))
        summary["projects"].append(analyzed)
        write(run_dir / "summary.partial.json", json.dumps(summary, indent=2, ensure_ascii=False))

    zlib = next((p for p in summary["projects"] if p["name"] == "zlib" and p["graph"]["parse_ok"]), None)
    if zlib:
        out_base = Path(summary["run_dir"]) / "zlib" / "graph" / "build_graph"
        py = root / "tools" / "venv" / "bin" / "python"
        py_exe = str(py) if py.exists() else "python3"
        rc, out = run_capture([py_exe, str(root / "tools" / "visualize_graph.py"), zlib["graph"]["path"], "-o", str(out_base), "-f", "svg", "--no-hash", "--max-label-len", "36"], root, timeout=180)
        write(Path(summary["run_dir"]) / "zlib" / "graph" / "visualize_graph.log", f"returncode={rc}\n\n{out}")
        if rc == 0 and Path(str(out_base) + ".svg").exists():
            zlib["graph_image"] = str(out_base) + ".svg"

    summary["finished_utc"] = now()
    write(summary["summary_json"], json.dumps(summary, indent=2, ensure_ascii=False))
    report(summary)
    return summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="/home/sprooc/reprobuild")
    ap.add_argument("--c-build-root", default="/home/sprooc/reprobuild/c_build")
    ap.add_argument("--result-root", default="/home/sprooc/reprobuild/results")
    ap.add_argument("--run-id", default="")
    ap.add_argument("--only", default="", help="comma-separated project names to run")
    args = ap.parse_args()
    summary = run_all(args)
    print(json.dumps({"run_dir": summary["run_dir"], "report": summary["report_md"], "summary": summary["summary_json"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
