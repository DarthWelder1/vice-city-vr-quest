# Source-only release checklist

The local `qbuild` directory is a private development/build tree containing a
complete reVC checkout. **Never publish or archive that directory.** The public
release root is this `miamivr-quest` repository only.

Before a tag or GitHub release:

1. Confirm `git diff --cached --name-only` contains no unexpected staged files.
2. Confirm the repository contains no APK/AAB/SO/EXE/DLL, signing key,
   `release-signing.properties`, game data, save, profiler CSV, log, Modern
   model overlay, `.codex*`, `.claude`, `.agents`, `build`, `dist` or cache.
3. Assemble from a clean public `mrxenginner/reVC` `miami` checkout at commit
   `026cd10f3fdbd92c089830e5067c4457c53c1b51` into a new output directory.
4. Assemble a second time with git's line-ending conversion switched off, which
   is how every builder outside Git for Windows runs:

   ```powershell
   $env:GIT_CONFIG_SYSTEM = "<path to an empty file>"
   $env:GIT_CONFIG_GLOBAL = "<path to an empty file>"
   ```

   Git for Windows ships `core.autocrlf=true` in its system config, and that
   rewrites patch line endings during `git apply`. A patch that only works
   because of it fails for everyone else. Each hunk must carry the line endings
   of the file it targets as reVC stores it: every reVC source is LF except
   `premake5.lua`, which is CRLF.
5. Build `:app:assembleDebug` from that new assembled tree with the toolchain
   listed in `BUILDING.md`. Do not treat an incremental `qbuild` build as the
   reproducibility test.
6. Inspect the generated APK locally. It should contain only application
   metadata/resources and the arm64 `libmiamivr.so`, `libc++_shared.so` and
   Khronos OpenXR loader. Do not add the APK to this source repository.
7. Publish the commit/tag only after reviewing the complete GitHub file list.
   If desired, use GitHub's automatic source archive; do not attach an APK,
   assembled reVC tree, retail data or a generated Modern model set.

The public build scripts intentionally refuse to overwrite an existing output
directory, and release signing intentionally fails unless the builder supplies
their own private keystore configuration.
