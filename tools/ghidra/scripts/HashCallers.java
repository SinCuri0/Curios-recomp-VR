import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

public class HashCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        try (PrintWriter w = new PrintWriter(a[0])) {
            for (int t = 1; t < a.length; t++) {
                Address target = toAddr(a[t]);
                w.println("== " + target);
                for (Reference r : getReferencesTo(target)) {
                    Address call = r.getFromAddress();
                    Function f = getFunctionContaining(call);
                    long value = -1;
                    for (int back = 1; back <= 16 && value < 0; back++) {
                        Address p = call.subtract(4L * back);
                        if (f != null && !f.getBody().contains(p)) break;
                        int word = getInt(p);
                        if ((word & 0x1F) != 1) continue;
                        if ((word & 0xFF800000) == 0x72800000) {
                            long k = ((word >>> 5) & 0xFFFF) << (16 * ((word >>> 21) & 3));
                            for (int z = back + 1; z <= back + 6; z++) {
                                int w2 = getInt(call.subtract(4L * z));
                                if ((w2 & 0xFF800000) == 0x52800000 && (w2 & 0x1F) == 1) {
                                    value = k | (((long) (w2 >>> 5) & 0xFFFF) << (16 * ((w2 >>> 21) & 3)));
                                    break;
                                }
                            }
                        } else if ((word & 0xFF800000) == 0x52800000) {
                            value = ((word >>> 5) & 0xFFFF) << (16 * ((word >>> 21) & 3));
                        }
                    }
                    Set<String> strs = new TreeSet<>();
                    if (f != null) {
                        for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                            for (Reference ir : i.getReferencesFrom()) {
                                Data d = getDataAt(ir.getToAddress());
                                if (d == null) continue;
                                StringDataInstance s = StringDataInstance.getStringDataInstance(d);
                                if (s != StringDataInstance.NULL_INSTANCE && s.getStringValue() != null) strs.add(s.getStringValue());
                            }
                        }
                    }
                    w.println(String.format("%08x  call %s  in %s  %s", value & 0xFFFFFFFFL, call,
                              f == null ? "-" : f.getEntryPoint(), String.join(" | ", strs)));
                }
            }
        }
    }
}
