// Ghidra headless script: decompile the functions containing the given addresses and write C to files.
// Run through tools/decomp.py. Arguments: output directory, then addresses (hex, absolute or exe-relative).
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class Decomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        outDir.mkdirs();
        long base = currentProgram.getImageBase().getOffset();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            long value = Long.parseUnsignedLong(args[i].replace("0x", ""), 16);
            if (value < base) value += base;                 // exe-relative
            Address addr = toAddr(value);
            Function fn = getFunctionContaining(addr);
            if (fn == null) {                                // code the analysis did not make a function of
                disassemble(addr);
                fn = createFunction(addr, null);
            }
            File out = new File(outDir, String.format("%x.c", value - base));
            try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
                if (fn == null) {
                    w.println("// no function at " + addr);
                    continue;
                }
                w.println("// " + fn.getName() + " at exe+" + Long.toHexString(fn.getEntryPoint().getOffset() - base)
                        + " (asked for exe+" + Long.toHexString(value - base) + ")");
                w.print("// called from:");
                int n = 0;
                for (Reference r : getReferencesTo(fn.getEntryPoint())) {
                    if (n++ >= 40) { w.print(" ..."); break; }
                    Function caller = getFunctionContaining(r.getFromAddress());
                    w.print(" exe+" + Long.toHexString(r.getFromAddress().getOffset() - base)
                            + (caller != null ? "(" + caller.getName() + ")" : "") + "/" + r.getReferenceType());
                }
                w.println();
                DecompileResults res = decomp.decompileFunction(fn, 120, monitor);
                if (res.decompileCompleted()) w.println(res.getDecompiledFunction().getC());
                else w.println("// decompile failed: " + res.getErrorMessage());
            }
            println("wrote " + out);
        }
        decomp.dispose();
    }
}
