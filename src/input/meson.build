# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, §4 Phase 2. No GPL-3.0 sources.
#
# input/ — libinput/wl_seat -> MotionEvent/KeyEvent pipelines; IME via
# zwp_text_input_v3. Event-driven dispatch, no fixed-timeout polls.
module_libs += static_library(
  'acl_input',
  files('input.cpp'),
  include_directories : config_inc,
)
