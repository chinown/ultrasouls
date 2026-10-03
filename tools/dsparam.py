"""Minimal reader/patcher for Dark Souls Remastered GameParam (DCX -> BND3 -> PARAM + PARAMDEF).

Field edits are done in place on the decompressed binder. Adding rows rebuilds the binder.
"""
import struct
import zlib
import re

DCX_HEADER_SIZE = 0x4C


def dcx_decompress(data):
    assert data[:4] == b"DCX\0" and data[0x28:0x2C] == b"DFLT", "unsupported DCX"
    usize, csize = struct.unpack_from(">II", data, 0x1C)
    # decompressobj tolerates streams some mod tools write without a proper trailer
    out = zlib.decompressobj().decompress(data[DCX_HEADER_SIZE:DCX_HEADER_SIZE + csize])
    assert len(out) == usize
    return out


def dcx_compress(header_src, raw):
    comp = zlib.compress(raw, 9)
    head = bytearray(header_src[:DCX_HEADER_SIZE])
    struct.pack_into(">II", head, 0x1C, len(raw), len(comp))
    return bytes(head) + comp


def _cstr(buf, off, enc="shift_jis"):
    end = buf.index(b"\0", off)
    return buf[off:end].decode(enc, errors="replace")


def _rev_bits(b):
    return int("{:08b}".format(b)[::-1], 2)


class Bnd3:
    """Read-only view of entries; data is patched in place through `buf`."""

    def __init__(self, raw):
        self.buf = bytearray(raw)
        b = self.buf
        assert b[:4] == b"BND3"
        raw_fmt, big, bitbig = b[0x0C], b[0x0D], b[0x0E]
        assert not big
        reverse = bitbig or ((raw_fmt & 1) and not (raw_fmt & 0x80))
        fmt = raw_fmt if reverse else _rev_bits(raw_fmt)
        self.fmt = fmt
        count = struct.unpack_from("<i", b, 0x10)[0]
        pos = 0x20
        self.entries = []
        for _ in range(count):
            e = {}
            pos += 4
            e["size"] = struct.unpack_from("<i", b, pos)[0]; pos += 4
            if fmt & 0x10:
                e["offset"] = struct.unpack_from("<q", b, pos)[0]; pos += 8
            else:
                e["offset"] = struct.unpack_from("<I", b, pos)[0]; pos += 4
            if fmt & 0x02:
                e["id"] = struct.unpack_from("<i", b, pos)[0]; pos += 4
            if fmt & 0x0C:
                e["name"] = _cstr(b, struct.unpack_from("<i", b, pos)[0]); pos += 4
            if fmt & 0x20:
                pos += 4
            self.entries.append(e)

    def find(self, stem):
        for e in self.entries:
            base = re.split(r"[\\/]", e.get("name", ""))[-1]
            if base.split(".")[0].lower() == stem.lower():
                return e
        raise KeyError(stem)

    def stems(self):
        return [re.split(r"[\\/]", e.get("name", ""))[-1].split(".")[0] for e in self.entries]

    def data(self, e):
        return bytes(self.buf[e["offset"]:e["offset"] + e["size"]])


_FMT = {"s8": "b", "u8": "B", "s16": "h", "u16": "H", "s32": "i", "u32": "I", "f32": "f"}
_SIZE = {"s8": 1, "u8": 1, "s16": 2, "u16": 2, "s32": 4, "u32": 4, "f32": 4}


class Field:
    __slots__ = ("name", "type", "offset", "bit", "bits", "size", "display")


class ParamDef:
    def __init__(self, data):
        field_count, field_size = struct.unpack_from("<HH", data, 0x08)
        header_size = struct.unpack_from("<H", data, 0x04)[0]
        self.param_type = _cstr(data, 0x0C, "ascii")
        assert field_size == 0xB0, hex(field_size)
        self.fields = []
        off = 0
        bit_pos = None  # (type, byte offset, next bit)
        for i in range(field_count):
            p = header_size + i * field_size
            display = _cstr(data, p, "shift_jis")
            dtype = _cstr(data, p + 0x40, "ascii")
            byte_count = struct.unpack_from("<i", data, p + 0x64)[0]
            iname = _cstr(data, p + 0x8C, "ascii")
            f = Field()
            f.display, f.type, f.bit, f.bits = display, dtype, None, None
            m = re.match(r"^(\w+)\s*:\s*(\d+)$", iname)
            a = re.match(r"^(\w+)\s*\[\s*(\d+)\s*\]$", iname)
            if m and dtype in ("u8", "u16", "u32", "dummy8"):
                base = "u8" if dtype == "dummy8" else dtype
                width = int(m.group(2))
                limit = _SIZE[base] * 8
                if bit_pos is None or bit_pos[0] != base or bit_pos[2] + width > limit:
                    bit_pos = [base, off, 0]
                    off += _SIZE[base]
                f.name, f.type, f.offset = m.group(1), base, bit_pos[1]
                f.bit, f.bits, f.size = bit_pos[2], width, _SIZE[base]
                bit_pos[2] += width
            else:
                bit_pos = None
                f.name = a.group(1) if a else iname
                f.offset = off
                if dtype == "dummy8":
                    f.size = int(a.group(2)) if a else 1
                elif dtype in ("fixstr", "fixstrW"):
                    f.size = byte_count
                else:
                    f.size = _SIZE[dtype]
                off += f.size
            self.fields.append(f)
        self.row_size = off
        self.by_name = {f.name: f for f in self.fields}


class Param:
    """View onto one PARAM inside the binder buffer; get/set write straight to the buffer."""

    def __init__(self, buf, base, size, paramdef):
        self.buf, self.base, self.size, self.pdef = buf, base, size, paramdef
        strings_off, data_start = struct.unpack_from("<IH", buf, base)
        row_count = struct.unpack_from("<H", buf, base + 0x0A)[0]
        self.param_type = _cstr(buf, base + 0x0C, "ascii")
        self.rows = {}
        self.names = {}
        self.order = []
        for i in range(row_count):
            rid, doff, noff = struct.unpack_from("<III", buf, base + 0x30 + i * 12)
            self.rows[rid] = base + doff
            self.order.append(rid)
            if noff:
                try:
                    self.names[rid] = _cstr(buf, base + noff)
                except ValueError:
                    pass
        if row_count > 1:
            offs = sorted(self.rows.values())
            self.detected_row_size = offs[1] - offs[0]
        else:
            self.detected_row_size = strings_off - data_start

    def get(self, rid, name):
        f = self.pdef.by_name[name]
        p = self.rows[rid] + f.offset
        if f.type in ("dummy8", "fixstr", "fixstrW"):
            return bytes(self.buf[p:p + f.size])
        v = struct.unpack_from("<" + _FMT[f.type], self.buf, p)[0]
        if f.bits is not None:
            v = (v >> f.bit) & ((1 << f.bits) - 1)
        return v

    def set(self, rid, name, value):
        f = self.pdef.by_name[name]
        p = self.rows[rid] + f.offset
        fmt = "<" + _FMT[f.type]
        if f.bits is not None:
            cur = struct.unpack_from(fmt, self.buf, p)[0]
            mask = ((1 << f.bits) - 1) << f.bit
            value = (cur & ~mask) | ((int(value) << f.bit) & mask)
        struct.pack_into(fmt, self.buf, p, value)

    def row(self, rid):
        return {f.name: self.get(rid, f.name) for f in self.pdef.fields if f.type != "dummy8"}


class GameParam:
    def __init__(self, gameparam_path, paramdef_path):
        self.dcx = open(gameparam_path, "rb").read()
        self.bnd = Bnd3(dcx_decompress(self.dcx))
        defs = Bnd3(dcx_decompress(open(paramdef_path, "rb").read()))
        self.defs = {}
        for e in defs.entries:
            d = ParamDef(defs.data(e))
            self.defs[d.param_type] = d
        self._parse_params()

    def __getitem__(self, stem):
        return self.params[stem]

    def _parse_params(self):
        self.params = {}
        for e, stem in zip(self.bnd.entries, self.bnd.stems()):
            ptype = _cstr(self.bnd.buf, e["offset"] + 0x0C, "ascii")
            if ptype in self.defs:
                self.params[stem] = Param(self.bnd.buf, e["offset"], e["size"], self.defs[ptype])

    def add_rows(self, stem, rows):
        """Append rows to a param. `rows` is a list of (new_id, source_id, name); each new row starts as a
        copy of the source row. This changes the file's size, so the whole binder is rebuilt and every
        Param object obtained before the call is stale: fetch them again with self[stem]."""
        p = self.params[stem]
        b = self.bnd.buf
        base, size = p.base, p.size
        old = bytes(b[base:base + size])
        strings_off, data_start = struct.unpack_from("<IH", old, 0)
        n = struct.unpack_from("<H", old, 0x0A)[0]
        rs = p.detected_row_size
        assert data_start == 0x30 + 12 * n and strings_off == data_start + n * rs, "unexpected PARAM layout"
        k = len(rows)
        for new_id, src, _ in rows:
            assert new_id not in p.rows, f"{stem} row {new_id} already exists"
            assert src in p.rows, f"{stem} source row {src} missing"
        new_data_start = 0x30 + 12 * (n + k)
        new_strings_off = new_data_start + (n + k) * rs
        shift_data = 12 * k
        shift_names = 12 * k + k * rs
        strings = bytearray(old[strings_off:])
        table = bytearray()
        for i in range(n):
            rid, doff, noff = struct.unpack_from("<III", old, 0x30 + i * 12)
            table += struct.pack("<III", rid, doff + shift_data, noff + shift_names if noff else 0)
        data = bytearray(old[data_start:strings_off])
        for j, (new_id, src, name) in enumerate(rows):
            src_off = p.rows[src] - base
            data += old[src_off:src_off + rs]
            name_off = new_strings_off + len(strings)
            strings += name.encode("shift_jis") + b"\0"
            table += struct.pack("<III", new_id, new_data_start + (n + j) * rs, name_off)
        head = bytearray(old[:0x30])
        struct.pack_into("<IH", head, 0, new_strings_off, new_data_start)
        struct.pack_into("<H", head, 0x0A, n + k)
        self._replace_file(base, bytes(head + table + data + strings))
        self._parse_params()

    def _replace_file(self, old_offset, new_bytes):
        """Rebuild the binder with one file's contents replaced; files stay 16-byte aligned."""
        bnd = self.bnd
        assert bnd.fmt == 0x2E, "binder layout not handled"
        b = bnd.buf
        order = sorted(range(len(bnd.entries)), key=lambda i: bnd.entries[i]["offset"])
        out = bytearray(b[:bnd.entries[order[0]]["offset"]])
        for i in order:
            e = bnd.entries[i]
            blob = new_bytes if e["offset"] == old_offset else bytes(b[e["offset"]:e["offset"] + e["size"]])
            while len(out) % 16:
                out.append(0)
            hdr = 0x20 + i * 24   # flags, size, offset, id, name offset, uncompressed size
            struct.pack_into("<I", out, hdr + 4, len(blob))
            struct.pack_into("<I", out, hdr + 8, len(out))
            struct.pack_into("<I", out, hdr + 20, len(blob))
            out += blob
        self.bnd = Bnd3(bytes(out))

    def save(self, path):
        open(path, "wb").write(dcx_compress(self.dcx, bytes(self.bnd.buf)))
