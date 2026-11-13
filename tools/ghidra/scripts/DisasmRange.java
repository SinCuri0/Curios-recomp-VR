import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;
import java.io.PrintWriter;

public class DisasmRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        try (PrintWriter w = new PrintWriter(a[0])) {
            for (int i = 1; i + 1 < a.length; i += 2) {
                long lo = Long.parseUnsignedLong(a[i], 16), hi = Long.parseUnsignedLong(a[i + 1], 16);
                w.println("== " + a[i] + ".." + a[i + 1]);
                for (long p = lo; p < hi; p += 4) {
                    Instruction ins = getInstructionAt(toAddr(p));
                    w.printf("%x %08x %s%n", p, getInt(toAddr(p)), ins == null ? "?" : ins.toString());
                }
            }
        }
    }
}
