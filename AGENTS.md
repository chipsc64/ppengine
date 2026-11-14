# AGENTS.md: ppengine

## Read first

This repository targets the Apollo V4 platform. Before doing anything else,
read `../../ApolloKnowledgeBase/AGENTS.md` and follow it. It explains the
platform, the assumptions you must not make, and which knowledge base files
to read for your task. Paths in that file are relative to
`ApolloKnowledgeBase/`.

If `ApolloKnowledgeBase/` is empty, the submodule has not been initialized.
Ask the user to run `git submodule update --init` instead of proceeding
without it.

## This repository

- **What it is:** a port of ppengine, a 3D remake of the Pole Position
  arcade game (game logic reverse engineered from the arcade ROMs), from
  Linux/OpenSceneGraph/OpenAL to the Apollo V4 with SAGA (Maggie 3D, Arne
  audio). The plan and task list are in `AMIGA_PORT_PLAN.md`.
- **Kind:** game
- **Target core:** Core 12856+ (developer's V4)
- **Target model:** any V4 model
- **Target OS:** AmigaOS 3.9 (Coffin), because maggie.library is documented
  for AmigaOS 3.9 only
- **Execution mode:** os-takeover (the game); the headless reference test
  `ppengine_headless` is a plain CLI program
- **Languages:** C (the C++/OpenSceneGraph code is being replaced), 68080
  assembly where it pays

## Layout

| Folder | Content | Platform |
|---|---|---|
| `src/ppengine/` | Arcade game logic, integers only. Do not change behaviour without a reason and a note in the plan | any |
| `src/track_geometry/`, `src/geometry/` | Track built from the arcade curve data (floating point) | any |
| `src/platform/headless/` | Headless reference test (engine checksum per frame) | host and V4 |
| `src/platform/amiga/` | Main loop, takeover, SAGA display, keyboard, debug view | V4 |
| `src/scene/scene.c`, `src/scene/maggie/` | Scene logic and the Maggie renderer | V4 |
| `tools/convert_assets.py` | Model/texture/image/font/sound conversion (formats in its header) | host |
| `third_party/MaggieLibrary/` | maggie.library source, commit 64c78ef (headers used) | – |
| `src/scene/osg/`, `src/input/`, `src/audio/gorilla/`, `src/client.c` | Original Linux layers, to be replaced (phases 1–5) | Linux only |

## Relevant knowledge base areas

- `ApolloKnowledgeBase/os/os-takeover.md`
- `ApolloKnowledgeBase/os/libraries/maggie-library.md`,
  `ApolloKnowledgeBase/os/pitfalls/maggie-library-side-effects.md`
- `ApolloKnowledgeBase/core/saga-3d-maggie/`, `core/pitfalls/maggie-*.md`
- `ApolloKnowledgeBase/core/saga-registers/` (chunky plane, Arne channels)
- `ApolloKnowledgeBase/core/saga-audio-arne/`
- `ApolloKnowledgeBase/toolchain/` and `toolchain/apollolib/`

## Build and test

```sh
make compile          # Amiga build (GCC-6.50-Dev): build/ppengine, build/ppengine_headless
make assets           # game data (python3 + ffmpeg): build/data/
make check            # host build + run: build/headless_host.txt
```

The game runs at 640x480 by default; `LOWRES` gives 320x240 (both 60 Hz).
The game needs `data/` next to the executable (it loads `PROGDIR:data/`)
and `LIBS:maggie.library` (4.0 tested). With amigent: put `build/ppengine`
and `build/data` in `RAM:pp/`.

Render test without a takeover (amigent keeps working):
`RAM:pp/ppengine SNAPSHOT 301 2001` writes `RAM:ppsnap_<tick>.565` and
prints the render time per stage; add `BENCH` (cost per level of detail)
or `DEPTHTEST` (depth-buffer check). Convert a `.565` with a few lines of
Python (P565 header, then big-endian R5G6B5).

Reference test on the V4: copy `build/ppengine_headless` to the V4, run
`ppengine_headless >RAM:headless_v4.txt` and compare it with
`build/headless_host.txt`. The `frame` and `done` lines must be identical;
the `track` lines may differ in the last decimals (x87 versus 68881).

Build and fix all errors and warnings before presenting code. The original
Linux build is `Makefile.linux` (needs OpenSceneGraph and Gorilla Audio).

## Repository-specific rules

- Keep `src/ppengine/` platform independent and deterministic: the headless
  checksum must stay equal on host and V4.
- Hash or compare engine structs field by field, never as raw bytes: struct
  padding differs between the host and m68k.
- The arcade CPU worked in 16 bits: in C, cast 16-bit arithmetic back to
  `uint16_t` before comparing (see `src/ppengine/sub/common/1572.c`).
- No 64-bit shifts or divides in V4 code (known m68k GCC problem).
- NDK: the target is AmigaOS 3.9, but this GCC-6.50-Dev install only has
  NDK 3.2 headers (`ndk-include`). Call only functions that AmigaOS 3.9 has.

## Knowledge base feedback

When you find something missing or wrong in ApolloKnowledgeBase while working
here, tell the user and offer to draft a change following
`ApolloKnowledgeBase/CONTRIBUTING.md`.

Offer a lesson when the user says the project, a release or a milestone is
done, and right away when an assumption turns out wrong. Follow
`ApolloKnowledgeBase/AGENTS.md` section 8: draft it from the lesson template,
show it to the user, and only submit it (as a pull request from the
`ApolloKnowledgeBase/` submodule) with their approval.
