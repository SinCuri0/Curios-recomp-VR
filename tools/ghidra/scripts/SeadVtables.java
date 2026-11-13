import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class SeadVtables extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        try (PrintWriter w = new PrintWriter(a[0])) {
            for (int i = 1; i + 1 < a.length; i += 2) {
                String cls = a[i];
                Address slot = toAddr(a[i + 1]);
                Address rtti = toAddr(getLong(slot));
                w.println("== " + cls + " rtti slot " + slot + " -> " + rtti);
                Set<Address> fns = new TreeSet<>();
                for (Reference r : getReferencesTo(slot)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null && f.getBody().getNumAddresses() <= 160) fns.add(f.getEntryPoint());
                }
                for (Address fa : fns) {
                    for (Reference r : getReferencesTo(fa)) {
                        if (r.getReferenceType().isCall() || r.getReferenceType().isJump()) continue;
                        Address vt = r.getFromAddress();
                        if (getLong(vt) != fa.getOffset()) continue;
                        w.println("  checkDerived " + fa + " in table at " + vt);
                        for (int k = 0; k < 16; k++) {
                            long p = getLong(vt.add(8L * k));
                            Function f = getFunctionAt(toAddr(p));
                            if (f == null) { w.println("    [" + k + "] " + Long.toHexString(p) + (getMemoryBlock(toAddr(p)) != null && getMemoryBlock(toAddr(p)).isExecute() ? " (code, no function)" : " (not code)")); continue; }
                            w.println("    [" + k + "] " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses());
                        }
                    }
                }
            }
        }
    }
}
