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
    c.getMethod("hello").invoke(null);  // prints "hello aclcxx" (sget-object+println); void
    // Object model: new Counter(10); c.add(5); c.get() == 15
    Object counter = Class.forName("Counter").getMethod("useCounter").invoke(null);
    Object arr = c.getMethod("arraySum").invoke(null);  // 42 (arrays)
    Object tc = c.getMethod("tryCatch").invoke(null);   // 99 (try/catch/throw)
    Object si = c.getMethod("staticInt").invoke(null);  // 10 (sget/sput int)
    Object oa = c.getMethod("objArray").invoke(null);   // 5 (aget/aput-object)
    Object ws = c.getMethod("wideSum").invoke(null);    // 2 (long arithmetic)
    Object wl = c.getMethod("wideLoop").invoke(null);   // 10 (long at frame point)
    Object ia = c.getMethod("intArith").invoke(null);   // 10 (div/rem/bitwise)
    Object il = c.getMethod("intLit").invoke(null);     // 10 (/2addr, /lit, neg/not)
    Object ba = c.getMethod("byteArray").invoke(null);  // 42 (narrow byte array)
    Object lb = c.getMethod("longBits").invoke(null);   // 42 (long div/rem/shl/shr)
    Object wa = c.getMethod("wideArray").invoke(null);  // 42 (long array)
    Object wf = c.getMethod("wideField").invoke(null);  // 42 (long static field)
    Object cc = c.getMethod("checkCast").invoke(null);  // 5 (check-cast)
    Object it = c.getMethod("instTest").invoke(null);   // 42 (instance-of)
    if (args.length > 0) {
      java.nio.file.Files.write(java.nio.file.Paths.get(args[0]),
          (answer + "," + loopSum + "," + callSum + "," + strlen + "," + counter + "," + arr + "," + tc + "," + si + "," + oa + "," + ws + "," + wl + "," + ia + "," + il + "," + ba + "," + lb + "," + wa + "," + wf + "," + cc + "," + it).getBytes("UTF-8"));
    }
  }
}
