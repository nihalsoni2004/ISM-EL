"""Audit protected files, local documentation references and source hygiene."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import zipfile
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]


def guard(path):
    attr = getattr(path.lstat(), "st_file_attributes", 0)
    if path.is_symlink() or attr & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        raise ValueError(f"Unexpected link in protected scope: {path.relative_to(ROOT)}")


def report_walk_error(error):
    raise error


def hashes(roots):
    files = {}
    for root in roots:
        base = ROOT / root
        guard(base)
        for folder, subdirs, names in os.walk(base, onerror=report_walk_error):
            parent = Path(folder)
            for subdir in subdirs:
                guard(parent / subdir)
            for name in names:
                path = parent / name
                guard(path)
                with path.open("rb") as source:
                    files[path.relative_to(ROOT).as_posix()] = hashlib.file_digest(source, "sha256").hexdigest()
    return files


def write_json(path, data):
    with path.open("x", encoding="utf-8") as output:
        json.dump(data, output, indent=2)
        output.write("\n")


def capture(path):
    roots = ["data", "benchmarks/results", "benchmarks/plots"]
    roots.extend(d.relative_to(ROOT).as_posix() for d in sorted((ROOT / "build").iterdir())
                 if d.is_dir() and re.match(r"^(step\d+[-_]|api-storage$)", d.name))
    snapshot = hashes(roots)
    write_json(path, {"recorded_utc": datetime.now(timezone.utc).isoformat(), "roots": roots,
                      "files": [{"path": name, "sha256": digest} for name, digest in sorted(snapshot.items())]})
    print(f"Recorded {len(snapshot)} historical SHA-256 values.")


def verify(path, report):
    baseline = json.loads(path.read_text(encoding="utf-8-sig"))
    old = {item["path"]: item["sha256"].lower() for item in baseline["files"]}
    current = hashes(baseline["roots"])
    changes = [{"path": name, "before": checksum, "after": current.get(name)} for name, checksum in old.items()
               if current.get(name) != checksum]
    existing = {"/".join(name.split("/")[:3]) for name in old}
    known_run = re.compile(r"benchmarks/(?:results|plots)/(?:rebuild-\d{8}T\d{6}Z-[0-9a-f]+|final-demo-[0-9a-f]{12}(?:-analysis)?)")
    separate, unexpected = [], []
    for name in sorted(current.keys() - old.keys()):
        group = "/".join(name.split("/")[:3])
        (separate if group not in existing and known_run.fullmatch(group) else unexpected).append(name)
    matched = len(old) - len(changes)
    write_json(report, {"before": len(old), "matched": matched, "changed_or_missing": changes,
                        "separate_benchmark_outputs": separate, "unexpected_additions": unexpected})
    print(f"Historical SHA-256 matched {matched}/{len(old)}. Separate benchmark outputs: {len(separate)}.")
    for item in [*changes, *unexpected]:
        print(item)
    return not changes and not unexpected


def hygiene():
    errors, link_count = [], 0
    docs = [ROOT / "README.md", *sorted((ROOT / "docs").glob("*.md")), *sorted((ROOT / "benchmarks/docs").glob("*.md"))]
    for path in docs:
        body = re.sub(r"\x60{3}.*?\x60{3}", "", path.read_text(encoding="utf-8-sig"), flags=re.S)
        for target in re.findall(r"\]\(([^)\r\n]+)\)", body):
            link = unquote(target.strip("<>").split("#", 1)[0])
            if not link or re.match(r"[a-zA-Z][a-zA-Z\d+.-]*:", link):
                continue
            link_count += 1
            if not (path.parent / link).exists():
                errors.append(f"Missing local link in {path.relative_to(ROOT)}: {link}")
    tracked = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True)
    for name in filter(None, tracked.stdout.decode("utf-8").split("\0")):
        path = Path(name)
        unwanted = {".venv", "node_modules", ".pytest_cache", "__pycache__", "build", "dist"}
        if set(path.parts) & unwanted or (path.name.startswith(".env") and path.name != ".env.example"):
            errors.append(f"Generated or local environment file tracked: {name}")
        disk = ROOT / path
        if disk.is_file() and disk.stat().st_size > 10 * 1024**2:
            errors.append(f"Unexpected tracked file above 10 MiB: {name}")
    credentials = re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----|\b(?:sk_live_|ghp_|AKIA|AIza)[a-zA-Z0-9_-]{16,}\b")
    prefixes = ("src", "include", "backend/app", "frontend/src", "frontend/browser", "scripts")
    suffixes = {".h", ".cpp", ".py", ".ps1", ".js", ".jsx", ".mjs", ".json"}
    source_files = [file for prefix in prefixes for file in (ROOT / prefix).rglob("*")
                    if file.is_file() and file.suffix in suffixes and file.name != Path(__file__).name]
    for path in [*docs, ROOT / "backend/.env.example", *source_files]:
        if credentials.search(path.read_text(encoding="utf-8-sig")):
            errors.append(f"Potential embedded credential: {path.relative_to(ROOT)}")
    if not (ROOT / "third_party/nlohmann/LICENSE.MIT").is_file():
        errors.append("Missing nlohmann/json MIT notice.")
    sqlite_zip = next((ROOT / "third_party/sqlite").glob("sqlite-amalgamation-*.zip"))
    with zipfile.ZipFile(sqlite_zip) as archive:
        with archive.open(next(name for name in archive.namelist() if name.endswith("/sqlite3.c"))) as sqlite:
            license_text = sqlite.read(12000).lower()
    if all(term not in license_text for term in (b"disclaims copyright", b"public domain")):
        errors.append("Missing SQLite license text or blessing.")
    print(f"Checked {len(docs)} Markdown files, {link_count} local references, tracked files, and vendor notices.")
    for error in errors:
        print(error)
    return not errors


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("capture").add_argument("baseline", type=Path)
    comparison = commands.add_parser("verify")
    comparison.add_argument("baseline", type=Path)
    comparison.add_argument("report", type=Path)
    commands.add_parser("hygiene")
    args = parser.parse_args()
    if args.command == "capture":
        capture(args.baseline)
    elif args.command == "verify":
        raise SystemExit(0 if verify(args.baseline, args.report) else 1)
    else:
        raise SystemExit(0 if hygiene() else 1)

