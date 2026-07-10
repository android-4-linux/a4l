// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture. No GPL-3.0 sources.
//
// Reflectively invokes DEX->JVM translated methods loaded at runtime from the
// converted classpath and writes "<answer>,<loopSum>" to args[0], so the native
// side can confirm real translated bytecode executed: answer()=42 (const/return)
// and loopSum()=55 (a translated loop with branches).
public class Runner {
  public static void main(String[] args) throws Exception {
    Class<?> c = Class.forName("HelloDex");
    Object answer = c.getMethod("answer").invoke(null);      // 42
    Object loopSum = c.getMethod("loopSum").invoke(null);    // 55 (branches)
    Object callSum = c.getMethod("callSum").invoke(null);    // 97 (invoke-static)
    Object strlen = c.getMethod("strlen").invoke(null);      // 5 (const-string+invoke-virtual)
    if (args.length > 0) {
      java.nio.file.Files.write(java.nio.file.Paths.get(args[0]),
          (answer + "," + loopSum + "," + callSum + "," + strlen).getBytes("UTF-8"));
    }
  }
}
