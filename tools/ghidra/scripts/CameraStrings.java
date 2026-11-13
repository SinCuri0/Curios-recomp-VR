import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;
import java.util.regex.Pattern;

public class CameraStrings extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "/tmp/camera_strings.tsv";
        Pattern want = Pattern.compile("(?i)camera|fovy|projection|lookat|viewmtx|view_mtx|nearclip|farclip");
        Listing listing = currentProgram.getListing();
        ReferenceManager refs = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        Map<Function, Set<String>> byFunc = new TreeMap<>(Comparator.comparing(Function::getEntryPoint));
        int strings = 0, matched = 0, unreferenced = 0;
        try (PrintWriter w = new PrintWriter(out)) {
            w.println("string_addr\tstring\tref_addr\tfunction\tfunction_size");
            for (Data d : listing.getDefinedData(true)) {
                if (monitor.isCancelled()) break;
                StringDataInstance s = StringDataInstance.getStringDataInstance(d);
                if (s == StringDataInstance.NULL_INSTANCE) continue;
                strings++;
                String v = s.getStringValue();
                if (v == null || !want.matcher(v).find()) continue;
                matched++;
                boolean any = false;
                for (Reference r : refs.getReferencesTo(d.getAddress())) {
                    any = true;
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    String fn = f == null ? "-" : f.getEntryPoint().toString();
                    long size = f == null ? 0 : f.getBody().getNumAddresses();
                    w.printf("%s\t%s\t%s\t%s\t%d%n", d.getAddress(), v.replace('\t', ' ').replace('\n', ' '),
                             r.getFromAddress(), fn, size);
                    if (f != null) byFunc.computeIfAbsent(f, k -> new TreeSet<>()).add(v);
                }
                if (!any) unreferenced++;
            }
        }
        println("functions " + fm.getFunctionCount() + ", strings " + strings + ", camera strings " + matched
                + " (" + unreferenced + " with no reference), functions using them " + byFunc.size());
        try (PrintWriter w = new PrintWriter(out.replace(".tsv", "_by_function.tsv"))) {
            w.println("function\tsize\tcount\tstrings");
            for (Map.Entry<Function, Set<String>> e : byFunc.entrySet()) {
                w.printf("%s\t%d\t%d\t%s%n", e.getKey().getEntryPoint(), e.getKey().getBody().getNumAddresses(),
                         e.getValue().size(), String.join(" | ", e.getValue()));
            }
        }
    }
}
