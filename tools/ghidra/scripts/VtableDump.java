// Prints the vtables that hold a function: for each data reference to it, the table from its first code entry.
// Args: <out.txt> <count> <function address or @table address>...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

public class VtableDump extends GhidraScript {
    private boolean isCode(long p) {
        MemoryBlock b = getMemoryBlock(toAddr(p));
        return b != null && b.isExecute();
    }

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        int count = Integer.parseInt(a[1]);
        try (PrintWriter w = new PrintWriter(a[0])) {
            for (int i = 2; i < a.length; i++) {
                if (a[i].startsWith("@")) {
                    Address start = toAddr(a[i].substring(1));
                    w.println("== table at " + start);
                    for (int k = 0; k < count; k++) {
                        long p = getLong(start.add(8L * k));
                        if (!isCode(p)) break;
                        Function f = getFunctionAt(toAddr(p));
                        w.println("    [" + k + "] " + Long.toHexString(p) + (f == null ? "" : " size " + f.getBody().getNumAddresses()));
                    }
                    continue;
                }
                Address fn = toAddr(a[i]);
                w.println("== " + fn);
                for (Reference r : getReferencesTo(fn)) {
                    if (r.getReferenceType().isCall() || r.getReferenceType().isJump()) continue;
                    Address at = r.getFromAddress();
                    if (getLong(at) != fn.getOffset()) continue;
                    Address start = at;
                    for (int k = 0; k < 120 && isCode(getLong(start.subtract(8))); k++) start = start.subtract(8);
                    w.println("  table at " + start + " (entry " + at.subtract(start) / 8 + " is this function)");
                    for (int k = 0; k < count; k++) {
                        long p = getLong(start.add(8L * k));
                        if (!isCode(p)) break;
                        Function f = getFunctionAt(toAddr(p));
                        w.println("    [" + k + "] " + Long.toHexString(p) + (f == null ? "" : " size " + f.getBody().getNumAddresses()));
                    }
                }
            }
        }
    }
}
