// Ghidra headless script: list the functions that refer to symbols whose name contains the given text
// (for example an RTTI vtable such as "hkpClosestRayHitCollector"). Run through tools/decomp.py --refs.
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class FindRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        out.getParentFile().mkdirs();
        long base = currentProgram.getImageBase().getOffset();
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                String needle = args[i].toLowerCase();
                SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
                int shown = 0;
                while (it.hasNext() && shown < 40) {
                    Symbol sym = it.next();
                    if (!sym.getName(true).toLowerCase().contains(needle)) continue;
                    shown++;
                    w.println(sym.getName(true) + " at exe+" + Long.toHexString(sym.getAddress().getOffset() - base));
                    int n = 0;
                    for (Reference r : getReferencesTo(sym.getAddress())) {
                        if (n++ >= 30) { w.println("    ..."); break; }
                        Function f = getFunctionContaining(r.getFromAddress());
                        w.println("    from exe+" + Long.toHexString(r.getFromAddress().getOffset() - base)
                                + (f != null ? " in " + f.getName() + " (exe+" + Long.toHexString(f.getEntryPoint().getOffset() - base) + ")" : "")
                                + " " + r.getReferenceType());
                    }
                }
            }
        }
        println("wrote " + out);
    }
}
