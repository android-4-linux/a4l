// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture. No GPL-3.0 sources.
//
// Test fixture: writes a marker to the file named by args[0] so the native side
// can confirm main() actually executed through the JNI Invocation API.
public class Main {
  public static void main(String[] args) throws Exception {
    if (args.length > 0) {
      java.nio.file.Files.write(java.nio.file.Paths.get(args[0]),
                                "aclcxx-jvm-ok".getBytes("UTF-8"));
    }
    System.out.println("Main.main ran with " + args.length + " arg(s)");
  }
}
