# Third-party notices

- **librw** — MIT License, copyright aap. The `librw/` directory is a fork;
  its original license is preserved at `librw/LICENSE`. The Vulkan backend
  (`librw/src/vulkan/`) was written for this project.
- **OpenXR** — the loader is fetched at build time as the official Khronos
  `org.khronos.openxr:openxr_loader_for_android` package. The repository also
  includes Khronos OpenXR 1.1.58 headers. Both are Apache License 2.0; the
  bundled header license is preserved under `overlay/vendor/openxr-1.1.58/`.
- **Qualcomm SGSR2 research code** — adapted under the BSD 3-Clause License.
  It is disabled in the release player path; the original notice is preserved
  at `librw/src/vulkan/SGSR_LICENSE.txt`.
- **VR hand meshes** (`overlay/gamefiles/models/vrhands/`) — baked from the
  UltimateXR SDK hand models, copyright VRMADA, MIT License.
- **This repository contains no complete reVC source tree and no assets from
  the original game or its publishers.** The patch files contain only the
  modifications and context required to transform a user-supplied reVC tree.
