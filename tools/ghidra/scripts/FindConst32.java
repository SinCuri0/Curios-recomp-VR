import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import java.io.PrintWriter;
import java.util.*;

public class FindConst32 extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Set<Long> want = new HashSet<>();
        for (int i = 1; i < a.length; i++) want.add(Long.parseLong(a[i], 16));
        try (PrintWriter w = new PrintWriter(a[0])) {
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                if (!b.isExecute()) continue;
                Address start = b.getStart();
                byte[] bytes = new byte[(int) b.getSize()];
                b.getBytes(start, bytes);
                java.nio.ByteBuffer buf = java.nio.ByteBuffer.wrap(bytes).order(java.nio.ByteOrder.LITTLE_ENDIAN);
                long n = b.getSize() / 4;
                long[] last = new long[32];
                Arrays.fill(last, -1);
                for (long k = 0; k < n; k++) {
                    Address p = start.add(4 * k);
                    int word = buf.getInt((int) (4 * k));
                    int rd = word & 0x1F;
                    int op = word & 0xFF800000;
                    if (op == 0x52800000) last[rd] = ((long) (word >>> 5) & 0xFFFF) << (16 * ((word >>> 21) & 3));
                    else if (op == 0x72800000 && last[rd] >= 0) {
                        int sh = 16 * ((word >>> 21) & 3);
                        long v = (last[rd] & ~(0xFFFFL << sh)) | (((long) (word >>> 5) & 0xFFFF) << sh);
                        last[rd] = v;
                        if (want.isEmpty() || want.contains(v)) {
                            Function f = getFunctionContaining(p);
                            w.println(String.format("%08x  w%d  at %s  in %s", v, rd, p, f == null ? "-" : f.getEntryPoint()));
                        }
                    }
                }
            }
        }
    }
}
