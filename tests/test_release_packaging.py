"""Packaging protocol regressions; optional --setup validates real PE payload ranges."""
from pathlib import Path
import hashlib
import json
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "ci"))
from payload_index import payload_index
from portable_list import rewrite_file_list, check


def fixture(entries):
    result = b"PTSETUP1" + struct.pack("<I", len(entries))
    for name, raw in entries:
        encoded, packed = name.encode(), zlib.compress(raw)
        result += (struct.pack("<H", len(encoded)) + encoded + struct.pack("<QQ", len(raw), len(packed))
                   + hashlib.sha256(raw).hexdigest().encode() + packed)
    return result


class Packaging(unittest.TestCase):
    def test_ranges_and_malformed_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "setup"
            payload = fixture([("test.txt", b"range data"), ("empty", b"")])
            prefix = b"ELF test stub with PTSETUP1 decoy"
            p.write_bytes(prefix + payload + b"PTPAYLD1" + struct.pack("<QQ", len(prefix), len(payload)))
            index = payload_index(p)
            self.assertEqual([e["path"] for e in index], ["test.txt", "empty"])
            for entry, raw in zip(index, [b"range data", b""]):
                self.assertEqual(zlib.decompress(p.read_bytes()[entry["offset"]:entry["offset"] + entry["packed"]]), raw)
            for invalid in [payload[:-1], payload + b"junk", fixture([("../escape", b"x")]),
                            fixture([("A", b"1"), ("a", b"2")]), fixture([("C:/escape", b"x")])]:
                p.write_bytes(invalid)
                with self.assertRaises(ValueError):
                    payload_index(p)

    def test_portable_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            p, manifest = Path(directory) / "runtime.zip", Path(directory) / "manifest.json"
            with zipfile.ZipFile(p, "w") as z:
                z.writestr("game/test.txt", b"test")
                z.writestr("game/pt-files.json", b"stale")
            rewrite_file_list(p)
            with zipfile.ZipFile(p) as z:
                listing = z.read("game/pt-files.json")
                expected = {name.split("/", 1)[1]: {"bytes": z.getinfo(name).file_size, "sha256": hashlib.sha256(z.read(name)).hexdigest()} for name in z.namelist()}
            manifest.write_text(json.dumps(expected))
            self.assertEqual(check(p, manifest), [])
            self.assertEqual(json.loads(listing)["files"][0]["sha256"], hashlib.sha256(b"test").hexdigest())
            expected["test.txt"]["sha256"] = "0" * 64
            manifest.write_text(json.dumps(expected))
            self.assertEqual(check(p, manifest), ["mismatched test.txt"])


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--setup":
        path = Path(sys.argv[2])
        entries = payload_index(path)
        with path.open("rb") as stream:
            for entry in entries:
                stream.seek(entry["offset"])
                raw = zlib.decompress(stream.read(entry["packed"]))
                assert len(raw) == entry["size"] and hashlib.sha256(raw).hexdigest() == entry["sha256"], entry["path"]
        print(f"Verified {len(entries)} real installer payload ranges, decompressed sizes and hashes")
    else:
        unittest.main()
