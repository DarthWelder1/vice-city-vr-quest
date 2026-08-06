# MiamiVR — Quest port (source only)

A native Meta Quest 3 VR port layer for the reVC engine: Vulkan renderer,
OpenXR session with 6DOF head tracking, Android platform layer, first-person
and vehicle play at 72 fps.

**This repository distributes no game, no game assets and no binaries.**
It contains only:

| Layer | What it is | License |
|---|---|---|
| `librw/` | librw fork with a Vulkan backend written for this port | MIT (librw by aap; backend additions same terms) |
| `overlay/` | The port's own sources: Android app, OpenXR/Vulkan session, platform layer, VR gameplay layer | MIT |
| `patches/` | A diff the build applies to a reVC source tree that **you provide** | — |

To play you additionally need, and must obtain yourself:

1. **The reVC source tree** (final master state, September 2021). It is not
   included and will not be provided here.
2. **Your own copy of the game's data files** from a legitimately purchased
   copy of the original PC game. Nothing from the game ships in the APK; the
   app reads everything from your files pushed to the headset.

Build instructions: [BUILDING.md](BUILDING.md).
Port internals and data setup: [overlay/docs/QUEST_PORT.md](overlay/docs/QUEST_PORT.md).

## Status

Playable: city, traffic, pedestrians, vehicles, first person on foot and in
vehicles, theater mode for menus and cutscenes, in-headset debug overlay.
Not done: audio (OpenAL backend pending), some cutscene presentation, minor
effect artifacts.

## No affiliation

This is an unofficial fan project. It is not affiliated with, endorsed by, or
connected to the publishers or developers of the original game. All
trademarks belong to their owners. No original game code or content is
distributed here. If you enjoy the game, buy it.
