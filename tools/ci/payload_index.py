"""Locate installer payload streams for the original signed range-update protocol.

Windows: PE RT_RCDATA/100. Linux: PTPAYLD1 footer. Offsets are file offsets,
not RVAs; never search executable strings for a payload magic marker.
"""
from pathlib import Path
import mmap
import re
import struct
from signed_trailer import body_size


def payload_index(path: Path):
    limit = body_size(path)
    with path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        def number(fmt, offset, end=limit):
            size = struct.calcsize(fmt)
            if offset < 0 or offset + size > end:
                raise ValueError("Truncated installer structure")
            return struct.unpack_from(fmt, data, offset)

        if data[:8] == b"PTSETUP1":
            start, length = 0, limit
        elif data[:2] == b"MZ":
            pe, = number("<I", 0x3c)
            if data[pe:pe + 4] != b"PE\0\0":
                raise ValueError("Invalid PE signature")
            sections, = number("<H", pe + 6)
            optional_size, = number("<H", pe + 20)
            optional = pe + 24
            magic, = number("<H", optional)
            directories = optional + (112 if magic == 0x20b else 96 if magic == 0x10b else 0)
            if directories == optional or directories + 24 > optional + optional_size:
                raise ValueError("Invalid PE optional header")
            resource_rva, resource_size = number("<II", directories + 16)
            section_table = optional + optional_size

            def file_offset(rva, size):
                for i in range(sections):
                    _, address, raw_size, raw = number("<IIII", section_table + i * 40 + 8)
                    if address <= rva and rva - address + size <= raw_size:
                        offset = raw + rva - address
                        if offset + size <= limit:
                            return offset
                raise ValueError("PE resource is outside file-backed sections")

            base = file_offset(resource_rva, resource_size)
            end = base + resource_size

            def entries(relative):
                named, ids = number("<HH", base + relative + 12, end)
                result = []
                for i in range(named + ids):
                    result.append(number("<II", base + relative + 16 + i * 8, end))
                return result

            directory = 0
            for wanted in (10, 100):  # RT_RCDATA, setup payload resource ID
                matches = [value for key, value in entries(directory) if key == wanted]
                if len(matches) != 1 or not matches[0] & 0x80000000:
                    raise ValueError("Missing or ambiguous setup payload resource")
                directory = matches[0] & 0x7fffffff
            languages = entries(directory)
            if len(languages) != 1 or languages[0][1] & 0x80000000:
                raise ValueError("Expected one payload resource language")
            rva, length = number("<II", base + languages[0][1], end)
            start = file_offset(rva, length)
        else:
            if limit < 24 or data[limit - 24:limit - 16] != b"PTPAYLD1":
                raise ValueError("No installer payload footer")
            start, length = number("<QQ", limit - 16)
            if start + length != limit - 24:
                raise ValueError("Invalid appended payload range")
        end = start + length
        if end > limit or length < 12 or data[start:start + 8] != b"PTSETUP1":
            raise ValueError("Invalid payload bounds or magic")
        count, = number("<I", start + 8, end)
        if not 0 < count <= 100000:
            raise ValueError("Invalid payload file count")
        cursor = start + 12
        result, seen = [], set()
        for _ in range(count):
            name_length, = number("<H", cursor, end)
            cursor += 2
            if not name_length or cursor + name_length + 80 > end:
                raise ValueError("Truncated payload file header")
            name = data[cursor:cursor + name_length].decode("utf-8")
            cursor += name_length
            if (name.startswith("/") or "\\" in name or ":" in name or "\0" in name
                    or any(p in (".", "..", "") for p in name.split("/")) or name.casefold() in seen):
                raise ValueError("Unsafe or duplicate payload filename")
            seen.add(name.casefold())
            size, packed = number("<QQ", cursor, end)
            cursor += 16
            digest = data[cursor:cursor + 64].decode("ascii")
            cursor += 64
            if not re.fullmatch("[0-9a-f]{64}", digest) or not packed or cursor + packed > end or size > 512 << 20:
                raise ValueError("Invalid payload file range or digest")
            result.append({"path": name, "size": size, "sha256": digest, "offset": cursor, "packed": packed})
            cursor += packed
        if cursor != end:
            raise ValueError("Trailing payload data")
        return result
