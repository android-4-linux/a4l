// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture. No GPL-3.0 sources.
//
// Test fixture: main() throws, so the native side can confirm uncaught-exception
// handling maps to Status::RuntimeError.
public class Boom {
  public static void main(String[] args) {
    throw new RuntimeException("boom from Boom.main");
  }
}
