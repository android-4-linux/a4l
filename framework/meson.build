# SPDX-License-Identifier: Apache-2.0
# Provenance: ADR-0002; docs/framework-bootstrap.md §A2 (framework staging), §B; plan.md
# §3 ADR-2, §4 Phase 1/2. No GPL-3.0 sources.
#
# framework/ — the android.* boot classpath (ADR-0002). Two layers compiled together
# with javac into build/framework/classes/, which the runtime loads ahead of app code:
#   framework/shim/               acl source-compatible shims (SDK docs; never the predecessor)
#   third_party/aosp/frameworks/  vendored AOSP frameworks/base subset (empty today)
# Both are plain Java (the framework is NOT dex-translated — only the app takes the
# DEX->JVM path). This is the default --framework so `acl run <apk>` resolves android.*
# without the caller pointing at a directory. Built only when a JDK/javac is present;
# otherwise the boot classpath is empty and the runtime falls back to static main().

framework_boot_classes = disabler()
framework_default_dir = ''

javac_prog = have_jni ? find_program(java_home / 'bin' / 'javac', required : false) \
                      : disabler()

if javac_prog.found()
  # Discover every .java across both layers (absolute paths). Adding or removing a
  # framework/vendored source needs a reconfigure — the found set is the input set.
  fw_scan = run_command(
    'find',
    meson.project_source_root() / 'framework' / 'shim',
    meson.project_source_root() / 'third_party' / 'aosp' / 'frameworks',
    '-name', '*.java',
    check : false,
  )
  fw_sources = []
  foreach line : fw_scan.stdout().strip().split('\n')
    if line != ''
      fw_sources += line
    endif
  endforeach

  # javac compiles the shim + vendored layers together into one boot dir. A duplicate
  # fully-qualified class across layers is a javac error — by design (one class, one
  # layer; vendoring a real AOSP class means deleting its shim).
  framework_boot_classes = custom_target(
    'framework-classes',
    output : 'classes',
    command : [javac_prog, '-d', '@OUTPUT@'] + fw_sources,
    depend_files : fw_sources,
    build_by_default : true,
  )
  framework_default_dir = meson.current_build_dir() / 'classes'
  summary({'framework boot classpath' : '@0@ source(s) -> build/framework/classes'.format(fw_sources.length())})
endif
