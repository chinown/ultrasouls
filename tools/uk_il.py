"""Dump fields and IL of a class from ULTRAKILL's Assembly-CSharp.dll.

usage: python tools/uk_il.py NewMovement [MethodName ...]
Needs: pip install dnfile dncil
"""
import sys
import dnfile
from dncil.cil.body import CilMethodBody
from dncil.cil.error import MethodBodyFormatError
from dncil.clr.token import Token, StringToken, InvalidToken
from dncil.cil.body.reader import CilMethodBodyReaderBase

DLL = r"C:\Program Files (x86)\Steam\steamapps\common\ULTRAKILL\ULTRAKILL_Data\Managed\Assembly-CSharp.dll"


class Reader(CilMethodBodyReaderBase):
    def __init__(self, pe, row):
        self.pe = pe
        self.offset = pe.get_offset_from_rva(row.Rva)

    def read(self, n):
        data = self.pe.get_data(self.pe.get_rva_from_offset(self.offset), n)
        self.offset += n
        return data

    def tell(self):
        return self.offset

    def seek(self, offset):
        self.offset = offset
        return self.offset


def type_name(row):
    ns = getattr(row, "TypeNamespace", "")
    return (str(ns) + "." if ns else "") + str(row.TypeName)


def resolve(pe, tok):
    if isinstance(tok, StringToken):
        s = pe.net.user_strings.get(tok.rid)
        return repr(s.value) if s else "str?"
    if isinstance(tok, InvalidToken) or not isinstance(tok, Token):
        return str(tok)
    table = pe.net.mdtables.tables.get(tok.table)
    if table is None or tok.rid == 0 or tok.rid > table.num_rows:
        return hex(tok.value)
    row = table.rows[tok.rid - 1]
    name = getattr(row, "Name", None) or getattr(row, "TypeName", None)
    owner = ""
    cls = getattr(row, "Class", None)
    if cls is not None and getattr(cls, "row", None) is not None:
        owner = str(getattr(cls.row, "TypeName", "") or "") + "::"
    elif tok.table in (4, 6):   # Field, MethodDef: find the owning TypeDef
        lists = "FieldList" if tok.table == 4 else "MethodList"
        for td in pe.net.mdtables.TypeDef.rows:
            if any(i.row_index == tok.rid for i in getattr(td, lists)):
                owner = str(td.TypeName) + "::"
                break
    return owner + str(name) if name is not None else hex(tok.value)


def main():
    cls, wanted = sys.argv[1], set(sys.argv[2:])
    pe = dnfile.dnPE(DLL)
    for td in pe.net.mdtables.TypeDef.rows:
        if str(td.TypeName) != cls:
            continue
        print("class", type_name(td))
        if not wanted:
            print("fields:", ", ".join(str(f.row.Name) for f in td.FieldList))
        for m in td.MethodList:
            row = m.row
            if wanted and str(row.Name) not in wanted:
                continue
            if not row.Rva:
                continue
            try:
                body = CilMethodBody(Reader(pe, row))
            except MethodBodyFormatError as e:
                print("  ", row.Name, "unreadable:", e)
                continue
            print(f"\n--- {row.Name} ({len(body.instructions)} instructions)")
            if not wanted:
                continue
            for ins in body.instructions:
                op = ins.operand
                text = resolve(pe, op) if isinstance(op, (Token, StringToken, InvalidToken)) else ("" if op is None else str(op))
                print(f"  {ins.offset:04X} {ins.mnemonic:12} {text}")


if __name__ == "__main__":
    main()
