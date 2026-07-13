// SPDX-License-Identifier: Apache-2.0
// Provenance: acl host-stub generator. Reflects an abstract framework class and emits a
// concrete subclass defaulting every abstract method (for Managers acl can't back without
// Binder, ADR-3). Run on the host JVM against the framework classes.jar. No GPL-3.0 sources.
import java.lang.reflect.*;
import java.util.*;

// Generate a concrete stub subclass of an abstract framework class (e.g. PackageManager),
// implementing every abstract method with a type-appropriate default. Run against the
// framework classes.jar so hidden abstract methods are visible.
public class GenStub {
    public static void main(String[] a) throws Exception {
        String cls = a[0];               // e.g. android.content.pm.PackageManager
        String pkg = a[1];               // e.g. org.acl.internal
        String name = a[2];              // e.g. StubPackageManager
        Class<?> c = Class.forName(cls);
        StringBuilder sb = new StringBuilder();
        sb.append("package ").append(pkg).append(";\n");
        sb.append("@SuppressWarnings({\"unchecked\",\"rawtypes\",\"deprecation\"})\n");
        sb.append("public class ").append(name).append(" extends ").append(cls).append(" {\n");
        // Collect abstract methods across the hierarchy, dedup by name+erased params.
        LinkedHashMap<String, Method> abs = new LinkedHashMap<>();
        Set<String> concrete = new HashSet<>();
        for (Class<?> k = c; k != null && k != Object.class; k = k.getSuperclass()) {
            for (Method m : k.getDeclaredMethods()) {
                if (m.isSynthetic() || m.isBridge()) continue;
                String key = sig(m);
                if (Modifier.isAbstract(m.getModifiers())) { if (!concrete.contains(key)) abs.putIfAbsent(key, m); }
                else concrete.add(key);
            }
        }
        for (Method m : abs.values()) {
            if (concrete.contains(sig(m))) continue;
            sb.append("  public ").append(tn(m.getReturnType())).append(' ')
              .append(m.getName()).append('(');
            Class<?>[] ps = m.getParameterTypes();
            for (int i = 0; i < ps.length; i++) {
                if (i > 0) sb.append(", ");
                sb.append(tn(ps[i])).append(" p").append(i);
            }
            sb.append(')');
            Class<?>[] ex = m.getExceptionTypes();
            if (ex.length > 0) {
                sb.append(" throws ");
                for (int i = 0; i < ex.length; i++) { if (i>0) sb.append(", "); sb.append(tn(ex[i])); }
            }
            sb.append(" { ").append(ret(m.getReturnType())).append(" }\n");
        }
        sb.append("}\n");
        System.out.print(sb);
    }
    static String sig(Method m) {
        StringBuilder s = new StringBuilder(m.getName()).append('(');
        for (Class<?> p : m.getParameterTypes()) s.append(tn(p)).append(',');
        return s.append(')').toString();
    }
    static String tn(Class<?> t) {
        if (t.isArray()) return tn(t.getComponentType()) + "[]";
        return t.getName().replace('$', '.');
    }
    static String ret(Class<?> t) {
        if (t == void.class) return "";
        if (t == boolean.class) return "return false;";
        if (t == char.class) return "return 0;";
        if (t.isPrimitive()) return "return 0;";
        return "return null;";
    }
}
