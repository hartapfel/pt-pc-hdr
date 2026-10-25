"""Keep the portable ZIP's installed-file manifest identical to the setup payload."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import shutil
import zipfile


def contents(archive):
    files = {}
    root = None
    for entry in archive.infolist():
        if entry.is_dir():
            continue
        parts = PurePosixPath(entry.filename).parts
        if len(parts) < 2 or ".." in parts or entry.filename.startswith("/") or "\\" in entry.filename:
            raise ValueError("Unsafe portable path: " + entry.filename)
        if root is None:
            root = parts[0]
        if parts[0] != root:
            raise ValueError("Portable ZIP must have one root directory")
        name = "/".join(parts[1:])
        if name in files:
            raise ValueError("Duplicate portable file: " + name)
        digest = hashlib.sha256()
        with archive.open(entry) as stream:
            for block in iter(lambda: stream.read(1 << 20), b""):
                digest.update(block)
        files[name] = {"bytes": entry.file_size, "sha256": digest.hexdigest()}
    if root is None:
        raise ValueError("Empty portable ZIP")
    return root, files


def rewrite_file_list(path: Path):
    temporary = path.with_suffix(path.suffix + ".new")
    if temporary.exists():
        raise FileExistsError(temporary)
    with zipfile.ZipFile(path) as source:
        root, files = contents(source)
        listing = {"format": 1, "files": [
            {"path": name, "size": entry["bytes"], "sha256": entry["sha256"]}
            for name, entry in sorted(files.items()) if name != "pt-files.json"]}
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as target:
            for entry in source.infolist():
                if entry.filename != root + "/pt-files.json":
                    with source.open(entry) as inp, target.open(entry, "w") as out:
                        shutil.copyfileobj(inp, out, 1 << 20)
            target.writestr(root + "/pt-files.json", (json.dumps(listing, indent=1, ensure_ascii=False) + "\n").encode("utf-8"))
    temporary.replace(path)


def check(path: Path, manifest: Path):
    expected = json.loads(manifest.read_text(encoding="utf-8"))
    with zipfile.ZipFile(path) as archive:
        _, actual = contents(archive)
    return (["missing " + n for n in sorted(expected.keys() - actual.keys())]
            + ["unexpected " + n for n in sorted(actual.keys() - expected.keys())]
            + ["mismatched " + n for n in sorted(actual.keys() & expected.keys()) if actual[n] != expected[n]])
