// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture. No GPL-3.0 sources.
//
// Reflectively invokes HelloDex.answer() (a DEX->JVM translated method loaded at
// runtime from the converted classpath) and writes its result to args[0], so the
// native side can confirm real translated bytecode executed and returned 42.
public class Runner {
  public static void main(String[] args) throws Exception {
    Class<?> c = Class.forName("HelloDex");
    Object r = c.getMethod("answer").invoke(null);
    if (args.length > 0) {
      java.nio.file.Files.write(java.nio.file.Paths.get(args[0]),
                                String.valueOf(r).getBytes("UTF-8"));
    }
  }
}
