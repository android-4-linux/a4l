<!-- SPDX-License-Identifier: Apache-2.0 -->
# specs/ — sanitized functional specifications

Output of the **specification compartment** (see `docs/clean-room.md`). Each spec describes
*behavior, interfaces, protocols, and test vectors* for one subsystem — never code excerpts,
paraphrased code, or file-structure mirroring of any GPL source.

Specs are the primary input the **implementation compartment** works from (alongside AOSP,
SDK docs, CTS, and black-box observation). A `src/` file's `// Provenance:` comment points
back to its spec here (e.g. `specs/runtime.md`).

- Copy `TEMPLATE.md` to start a new spec; keep the provenance header.
- One spec per subsystem, named to match the module (`runtime`, `pkg`, `wm`, `gfx`, `input`,
  `audio`, `media`, `svc`, `svc-car`, `ndk`, `shell`, `host-cal`, `host-media`).
- Specs pass the sanitization gate before implementers use them.
