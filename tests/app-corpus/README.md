# App corpus — real-app compatibility harness

Per `plan.md` §5. 10–15 redistributable F-Droid apps, **pinned by hash**, launched under
a screenshot + input-replay harness with a per-app status page. Golden-image rendering
diffs against CTS references where available.

APK binaries are **not** committed (see `.gitignore`: `tests/app-corpus/apks/`). A
manifest here pins each app by name, version, and SHA-256; tooling fetches them on demand.
Corpus selection favors no-NDK / no-GMS apps early (Gate G2), broadening per phase.
