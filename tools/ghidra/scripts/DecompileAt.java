import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                Function f = getFunctionContaining(toAddr(args[i]));
                if (f == null) { w.println("// no function at " + args[i]); continue; }
                w.println("// ==== " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses());
                w.print("// called from:");
                int n = 0;
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    if (n++ == 20) { w.print(" ..."); break; }
                    Function c = getFunctionContaining(r.getFromAddress());
                    w.print(" " + (c == null ? r.getFromAddress() : c.getEntryPoint()));
                }
                w.println();
                DecompileResults res = di.decompileFunction(f, 120, monitor);
                w.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed: " + res.getErrorMessage());
            }
        }
        di.dispose();
    }
}
