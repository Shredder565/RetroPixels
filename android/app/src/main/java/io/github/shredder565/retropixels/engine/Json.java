package io.github.shredder565.retropixels.engine;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A small JSON reader and writer for the engine's messages (plain Java, so the
 * protocol code runs on a computer's JVM in tests as well as on Android).
 * Objects are {@code Map<String,Object>}, arrays {@code List<Object>}, numbers
 * {@code Double}, plus {@code String}, {@code Boolean} and {@code null}.
 */
public final class Json {
    private final String s;
    private int i;

    private Json(String s) {
        this.s = s;
    }

    public static Object parse(String text) {
        Json j = new Json(text);
        j.ws();
        Object v = j.value();
        j.ws();
        if (j.i != j.s.length()) {
            throw new IllegalArgumentException("extra text after JSON at " + j.i);
        }
        return v;
    }

    @SuppressWarnings("unchecked")
    public static Map<String, Object> parseObject(String text) {
        Object v = parse(text);
        if (!(v instanceof Map)) {
            throw new IllegalArgumentException("not a JSON object");
        }
        return (Map<String, Object>) v;
    }

    private void ws() {
        while (i < s.length()) {
            char c = s.charAt(i);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                i++;
            } else {
                break;
            }
        }
    }

    private IllegalArgumentException bad(String what) {
        return new IllegalArgumentException("bad JSON (" + what + ") at " + i);
    }

    private Object value() {
        if (i >= s.length()) {
            throw bad("end");
        }
        char c = s.charAt(i);
        switch (c) {
            case '{': {
                i++;
                Map<String, Object> m = new LinkedHashMap<String, Object>();
                ws();
                if (i < s.length() && s.charAt(i) == '}') {
                    i++;
                    return m;
                }
                while (true) {
                    ws();
                    if (i >= s.length() || s.charAt(i) != '"') {
                        throw bad("key");
                    }
                    String k = string();
                    ws();
                    expect(':');
                    ws();
                    m.put(k, value());
                    ws();
                    if (i < s.length() && s.charAt(i) == ',') {
                        i++;
                        continue;
                    }
                    expect('}');
                    return m;
                }
            }
            case '[': {
                i++;
                List<Object> l = new ArrayList<Object>();
                ws();
                if (i < s.length() && s.charAt(i) == ']') {
                    i++;
                    return l;
                }
                while (true) {
                    ws();
                    l.add(value());
                    ws();
                    if (i < s.length() && s.charAt(i) == ',') {
                        i++;
                        continue;
                    }
                    expect(']');
                    return l;
                }
            }
            case '"':
                return string();
            case 't':
                word("true");
                return Boolean.TRUE;
            case 'f':
                word("false");
                return Boolean.FALSE;
            case 'n':
                word("null");
                return null;
            default:
                return number();
        }
    }

    private void expect(char c) {
        if (i >= s.length() || s.charAt(i) != c) {
            throw bad("expected " + c);
        }
        i++;
    }

    private void word(String w) {
        if (!s.startsWith(w, i)) {
            throw bad("expected " + w);
        }
        i += w.length();
    }

    private Double number() {
        int start = i;
        while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) {
            i++;
        }
        if (start == i) {
            throw bad("value");
        }
        try {
            return Double.valueOf(s.substring(start, i));
        } catch (NumberFormatException e) {
            throw bad("number");
        }
    }

    private String string() {
        expect('"');
        StringBuilder b = new StringBuilder();
        while (true) {
            if (i >= s.length()) {
                throw bad("unterminated string");
            }
            char c = s.charAt(i++);
            if (c == '"') {
                return b.toString();
            }
            if (c != '\\') {
                b.append(c);
                continue;
            }
            if (i >= s.length()) {
                throw bad("escape");
            }
            char e = s.charAt(i++);
            switch (e) {
                case '"': b.append('"'); break;
                case '\\': b.append('\\'); break;
                case '/': b.append('/'); break;
                case 'b': b.append('\b'); break;
                case 'f': b.append('\f'); break;
                case 'n': b.append('\n'); break;
                case 'r': b.append('\r'); break;
                case 't': b.append('\t'); break;
                case 'u':
                    if (i + 4 > s.length()) {
                        throw bad("escape");
                    }
                    b.append((char) Integer.parseInt(s.substring(i, i + 4), 16));
                    i += 4;
                    break;
                default:
                    throw bad("escape");
            }
        }
    }

    // ---- writing ----

    public static String write(Object v) {
        StringBuilder b = new StringBuilder();
        write(b, v);
        return b.toString();
    }

    @SuppressWarnings("unchecked")
    private static void write(StringBuilder b, Object v) {
        if (v == null) {
            b.append("null");
        } else if (v instanceof String) {
            quote(b, (String) v);
        } else if (v instanceof Boolean) {
            b.append(((Boolean) v).booleanValue() ? "true" : "false");
        } else if (v instanceof Double || v instanceof Float) {
            double d = ((Number) v).doubleValue();
            if (Double.isNaN(d) || Double.isInfinite(d)) {
                b.append('0');
            } else if (d == Math.rint(d) && Math.abs(d) < 1e15) {
                b.append((long) d);
            } else {
                b.append(d);
            }
        } else if (v instanceof Number) {
            b.append(((Number) v).longValue());
        } else if (v instanceof Map) {
            b.append('{');
            boolean first = true;
            for (Map.Entry<String, Object> e : ((Map<String, Object>) v).entrySet()) {
                if (!first) {
                    b.append(',');
                }
                first = false;
                quote(b, e.getKey());
                b.append(':');
                write(b, e.getValue());
            }
            b.append('}');
        } else if (v instanceof Iterable) {
            b.append('[');
            Iterator<Object> it = ((Iterable<Object>) v).iterator();
            boolean first = true;
            while (it.hasNext()) {
                if (!first) {
                    b.append(',');
                }
                first = false;
                write(b, it.next());
            }
            b.append(']');
        } else if (v instanceof int[]) {
            int[] a = (int[]) v;
            b.append('[');
            for (int k = 0; k < a.length; k++) {
                if (k > 0) {
                    b.append(',');
                }
                b.append(a[k]);
            }
            b.append(']');
        } else if (v instanceof String[]) {
            String[] a = (String[]) v;
            b.append('[');
            for (int k = 0; k < a.length; k++) {
                if (k > 0) {
                    b.append(',');
                }
                quote(b, a[k]);
            }
            b.append(']');
        } else {
            quote(b, v.toString());
        }
    }

    private static void quote(StringBuilder b, String s) {
        b.append('"');
        for (int k = 0; k < s.length(); k++) {
            char c = s.charAt(k);
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) {
                        b.append(String.format("\\u%04x", (int) c));
                    } else {
                        b.append(c);
                    }
            }
        }
        b.append('"');
    }

    // ---- reading values out of parsed objects ----

    public static String str(Map<String, Object> m, String k, String def) {
        Object v = m == null ? null : m.get(k);
        return v instanceof String ? (String) v : def;
    }

    public static double num(Map<String, Object> m, String k, double def) {
        Object v = m == null ? null : m.get(k);
        return v instanceof Number ? ((Number) v).doubleValue() : def;
    }

    public static int integer(Map<String, Object> m, String k, int def) {
        Object v = m == null ? null : m.get(k);
        return v instanceof Number ? (int) Math.round(((Number) v).doubleValue()) : def;
    }

    public static boolean bool(Map<String, Object> m, String k) {
        Object v = m == null ? null : m.get(k);
        return v instanceof Boolean && (Boolean) v;
    }

    @SuppressWarnings("unchecked")
    public static List<Object> list(Map<String, Object> m, String k) {
        Object v = m == null ? null : m.get(k);
        return v instanceof List ? (List<Object>) v : new ArrayList<Object>();
    }

    @SuppressWarnings("unchecked")
    public static Map<String, Object> obj(Map<String, Object> m, String k) {
        Object v = m == null ? null : m.get(k);
        return v instanceof Map ? (Map<String, Object>) v : new LinkedHashMap<String, Object>();
    }

    public static List<String> strings(Map<String, Object> m, String k) {
        List<String> out = new ArrayList<String>();
        for (Object o : list(m, k)) {
            if (o instanceof String) {
                out.add((String) o);
            }
        }
        return out;
    }

    /** A JSON object to build requests with: obj("cmd", "open", "path", p). */
    public static Map<String, Object> obj(Object... kv) {
        Map<String, Object> m = new LinkedHashMap<String, Object>();
        for (int k = 0; k + 1 < kv.length; k += 2) {
            m.put((String) kv[k], kv[k + 1]);
        }
        return m;
    }
}
