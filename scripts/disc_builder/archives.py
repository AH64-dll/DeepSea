"""Bounded Yaz0/RARC readers used only on the player's local disc data."""
import struct

MAX_SIZE = 64 * 1024 * 1024


def u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("Truncated archive header")
    return struct.unpack_from(">I", data, offset)[0]


def yaz0(data):
    if data[:4] != b"Yaz0":
        return data
    size = u32(data, 4)
    if size > MAX_SIZE or len(data) < 16:
        raise ValueError("Invalid Yaz0 size")
    result = bytearray()
    pos = 16
    while len(result) < size:
        if pos >= len(data):
            raise ValueError("Truncated Yaz0 stream")
        code = data[pos]
        pos += 1
        for bit in range(7, -1, -1):
            if len(result) == size:
                break
            if pos >= len(data):
                raise ValueError("Truncated Yaz0 block")
            if code & (1 << bit):
                result.append(data[pos])
                pos += 1
            else:
                if pos + 2 > len(data):
                    raise ValueError("Truncated Yaz0 reference")
                first, second = data[pos:pos + 2]
                pos += 2
                distance = ((first & 15) << 8) + second + 1
                count = first >> 4
                if count == 0:
                    if pos >= len(data):
                        raise ValueError("Truncated Yaz0 length")
                    count = data[pos] + 18
                    pos += 1
                else:
                    count += 2
                if distance > len(result) or count > size - len(result):
                    raise ValueError("Invalid Yaz0 back-reference")
                for _ in range(count):
                    result.append(result[-distance])
    return bytes(result)


def rarc_files(data):
    """Yield relative paths and decompressed contents, never write archive paths."""
    data = yaz0(data)
    if data[:4] != b"RARC" or u32(data, 4) != len(data):
        raise ValueError("Invalid RARC archive")
    node_count, node_offset = u32(data, 0x20), u32(data, 0x24) + 0x20
    entry_count, entry_offset = u32(data, 0x28), u32(data, 0x2c) + 0x20
    string_size, string_offset = u32(data, 0x30), u32(data, 0x34) + 0x20
    data_offset = u32(data, 0xc) + 0x20
    if (node_count > 4096 or entry_count > 65536 or
            node_offset + node_count * 16 > len(data) or
            entry_offset + entry_count * 20 > len(data) or
            string_offset + string_size > len(data)):
        raise ValueError("RARC tables outside archive")

    def walk(node, parent, ancestors):
        if node >= node_count or node in ancestors or len(ancestors) > 32:
            raise ValueError("Invalid RARC directory tree")
        offset = node_offset + node * 16
        count = int.from_bytes(data[offset + 10:offset + 12], "big")
        first = u32(data, offset + 12)
        if first + count > entry_count:
            raise ValueError("Invalid RARC entry range")
        for index in range(first, first + count):
            offset = entry_offset + index * 20
            kind_name = u32(data, offset + 4)
            name_offset = kind_name & 0xffff
            if name_offset >= string_size:
                raise ValueError("Invalid RARC name offset")
            end = data.find(b"\0", string_offset + name_offset, string_offset + string_size)
            if end < 0:
                raise ValueError("Unterminated RARC name")
            name = data[string_offset + name_offset:end].decode("ascii")
            if name in (".", ".."):
                continue
            if not name or any(c in name for c in "/\\:"):
                raise ValueError("Unsafe RARC filename")
            location, size = u32(data, offset + 8), u32(data, offset + 12)
            path = parent + (name,)
            if kind_name & 0x02000000:
                yield from walk(location, path, ancestors | {node})
            else:
                start = data_offset + location
                if size > MAX_SIZE or start + size > len(data):
                    raise ValueError("RARC file outside archive")
                yield "/".join(path), yaz0(data[start:start + size])

    yield from walk(0, (), set())
