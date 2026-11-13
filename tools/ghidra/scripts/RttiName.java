// Prints the class name of each vtable from its RTTI: vtable[-1] -> typeinfo, typeinfo+8 -> mangled name.
// Args: <out.txt> <vtable address>...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;

public class RttiName extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                Address vt = toAddr(args[i]);
                String name = "?";
                try {
                    Address ti = toAddr(getLong(vt.subtract(8)));
                    Address np = toAddr(getLong(ti.add(8)));
                    StringBuilder sb = new StringBuilder();
                    for (int k = 0; k < 200; k++) {
                        byte b = getByte(np.add(k));
                        if (b == 0) break;
                        sb.append((char) b);
                    }
                    name = sb.toString();
                } catch (Exception e) {
                    name = "? " + e.getMessage();
                }
                Address f0 = null;
                try { f0 = toAddr(getLong(vt)); } catch (Exception e) {}
                w.println(args[i] + " " + name + " first " + f0);
            }
        }
    }
}
