import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

public class RefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        try (PrintWriter w = new PrintWriter(a[0])) {
            for (int i = 1; i < a.length; i++) {
                Address t = toAddr(a[i]);
                w.println("== " + t);
                int n = 0;
                for (Reference r : getReferencesTo(t)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    w.println("  " + r.getFromAddress() + " " + r.getReferenceType() + " in " +
                              (f == null ? "-" : f.getEntryPoint() + " size " + f.getBody().getNumAddresses()));
                    if (++n >= 60) break;
                }
                for (int k = 0; k < 12; k++) {
                    long p = getLong(t.add(8L * k));
                    Function f = getFunctionAt(toAddr(p));
                    w.println("    [" + k + "] " + Long.toHexString(p) + (f == null ? "" : " size " + f.getBody().getNumAddresses()));
                }
            }
        }
    }
}
