# Third-Party Notices

## Zydis v4.1.1

rePIU vendors the official amalgamated source generated from Zydis tag `v4.1.1` (`a2278f1d254e492f6a6b39f6cb5d1f5d515659dc`) and its pinned Zycore submodule (`0b2432ced0884fd152b471d97ecf0258ff4d859f`). Both projects use the MIT License.

- Project: https://github.com/zyantific/zydis
- Zydis license: [`third_party/zydis/LICENSE-ZYDIS`](third_party/zydis/LICENSE-ZYDIS)
- Zycore license: [`third_party/zydis/LICENSE-ZYCORE`](third_party/zydis/LICENSE-ZYCORE)

Generated file SHA-256 values:

- `Zydis.c`: `1851E6D2EC6A681915D3723A96516A387F628ECA6E092A04BEDE78198CDF644A`
- `Zydis.h`: `CE154FC859C134C1DF5BC62A9DFE1C427135812C1A7BC1C54BA180EA27A70E55`

The vendored amalgamation is used only for x86 instruction decoding and metadata. Original PIU code continues to execute directly; Zydis does not replace game or resource-processing logic.

## Dear ImGui v1.92.9
rePIU fetches Dear ImGui v1.92.9 (commit `01380c579715e62fb9a8d6ec0502c4ea83bfde6e`) and builds
only its core sources with the `imgui_impl_sdl3` and `imgui_impl_opengl3` backends, which draw the
launcher and the in-game OSD inside the SDL3 windows the host already creates. The OSD and the
launcher draw with ProggyForever, the scalable default font embedded in Dear ImGui. Dear ImGui uses
the MIT License.
- Upstream: https://github.com/ocornut/imgui
- License text ships with the fetched sources as `LICENSE.txt`.

## libchdr v0.3.0

rePIU vendors libchdr tag `v0.3.0` (commit `93d8c239ff0d4e8d7722985992649fce12d2463b`) to read MAME CHDv1-v5 images. rePIU uses its read-only CHD hunk API and implements the pumpit1 ISO9660 mount separately.

- Project: https://github.com/rtissera/libchdr
- License: [`third_party/libchdr/LICENSE.txt`](third_party/libchdr/LICENSE.txt) (BSD 3-Clause)
- Bundled LZMA SDK is public domain as recorded in [`deps/lzma-25.01/LICENSE`](third_party/libchdr/deps/lzma-25.01/LICENSE).
- Bundled miniz uses the MIT license stated at the top of [`miniz.c`](third_party/libchdr/deps/miniz-3.1.1/miniz.c).
- Bundled zstd decompressor is dual-licensed; rePIU selects its BSD license option as stated in [`zstddeclib.c`](third_party/libchdr/deps/zstd-1.5.7/zstddeclib.c). No GPL option is used.

libchdr decodes storage assets only. It does not provide CPU, machine, game-logic, or full-system emulation.

## MAME PIU10 and CAT702 algorithms

### 한국어

rePIU의 플랫폼 공용 PIU10 ISA register와 CAT702 PIU 직렬 상태 모델은 MAME의
`xtom3d_piu10.cpp`(copyright windyfairy)와 `cat702.cpp`(copyright smf)를 참고하여
이식했습니다. 두 파일은 BSD 3-Clause License입니다. MAME emulator나 CPU/machine
framework는 통합하지 않았습니다.

- Project: https://github.com/mamedev/mame
- PIU10 source: https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d_piu10.cpp
- CAT702 source: https://github.com/mamedev/mame/blob/master/src/devices/machine/cat702.cpp

### English

rePIU's platform-neutral PIU10 ISA registers and CAT702 PIU serial state model are adapted from
MAME's `xtom3d_piu10.cpp` (copyright windyfairy) and `cat702.cpp` (copyright smf), both under the
BSD 3-Clause License. No MAME emulator, CPU, or machine framework is integrated.

Copyright (c) MAME contributors identified above. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of
   conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of
   conditions and the following disclaimer in the documentation and/or other materials provided
   with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## minimp3

### 한국어

rePIU는 PIU10 MAS3507D MP3 stream의 frame decode를 위해 upstream `minimp3` commit
`ea99364f61c14656440e8d77e9c233ccf3124633`을 FetchContent로 고정합니다. `minimp3`는
CC0 1.0 Universal로 제공됩니다. rePIU는 MAME의 MAS3507D 또는 MP3 wrapper 코드를
포함하지 않습니다.

- minimp3: https://github.com/lieff/minimp3
- 고정 commit: https://github.com/lieff/minimp3/commit/ea99364f61c14656440e8d77e9c233ccf3124633
- CC0 1.0: https://github.com/lieff/minimp3/blob/ea99364f61c14656440e8d77e9c233ccf3124633/LICENSE

### English

rePIU pins upstream `minimp3` commit
`ea99364f61c14656440e8d77e9c233ccf3124633` through FetchContent for frame decoding of the
PIU10 MAS3507D MP3 stream. `minimp3` is offered under CC0 1.0 Universal. rePIU does not
incorporate MAME's MAS3507D or MP3 wrapper code.

## SDL 3.4.10

### 한국어

rePIU는 창, 입력, 오디오, OpenGL context를 위해 SDL tag `release-3.4.10`을 FetchContent로
받아 정적으로 링크합니다. SDL은 zlib License로 제공됩니다. 소스는 수정하지 않습니다.

- Project: https://github.com/libsdl-org/SDL
- License: https://github.com/libsdl-org/SDL/blob/release-3.4.10/LICENSE.txt (zlib)

### English

rePIU fetches SDL tag `release-3.4.10` through FetchContent and links it statically for
windowing, input, audio and the OpenGL context. SDL is offered under the zlib License. The
sources are unmodified.

## spdlog v1.14.1

### 한국어

rePIU의 Windows·Linux loader 진단 로그는 spdlog tag `v1.14.1`을 FetchContent로 받아
header-only로 사용합니다(내장 fmt 포함). spdlog는 MIT License로 제공됩니다. Emscripten
빌드는 spdlog를 받지 않습니다.

- Project: https://github.com/gabime/spdlog
- License: https://github.com/gabime/spdlog/blob/v1.14.1/LICENSE (MIT)

### English

The Windows and Linux loader diagnostics use spdlog tag `v1.14.1`, fetched through
FetchContent and consumed header-only (with its bundled fmt). spdlog is offered under the
MIT License. The Emscripten build does not fetch it.

## GoogleTest v1.14.1

### 한국어

테스트 빌드는 GoogleTest tag `v1.14.1`을 FetchContent로 받습니다. GoogleTest는 BSD
3-Clause License로 제공됩니다. 테스트 실행 파일에만 링크되며 배포 패키지에는 포함되지
않습니다.

- Project: https://github.com/google/googletest
- License: https://github.com/google/googletest/blob/v1.14.1/LICENSE (BSD 3-Clause)

### English

The test build fetches GoogleTest tag `v1.14.1` through FetchContent. GoogleTest is
offered under the BSD 3-Clause License. It links into the test executables only and is not
part of the release packages.

## AMD FidelityFX Super Resolution 1.0 (built-in `fsr` screen shader)

### 한국어

내장 화면 shader `fsr`(issue #90)는 AMD FidelityFX Super Resolution 1.0의 EASU(업스케일)와 RCAS(선명화) 알고리즘을
`ffx_fsr1.h`에서 libretro 형식의 GLSL pass 둘로 옮긴 것입니다(`src/engine/post_shaders/fsr/`, re2DJ #26·#27의 파일).
FidelityFX FSR 1.0은 MIT License로 제공되며, 각 pass 파일 머리에 고지를 둡니다. AMD의 소스 파일 자체는 포함하지 않습니다.

- Project: https://github.com/GPUOpen-Effects/FidelityFX-FSR
- License: https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/master/license.txt (MIT)

### English

The built-in screen shader `fsr` (issue #90) ports the EASU (upscaling) and RCAS (sharpening) algorithms of AMD
FidelityFX Super Resolution 1.0 from `ffx_fsr1.h` into two libretro-style GLSL passes (`src/engine/post_shaders/fsr/`,
the files of re2DJ #26 and #27). FidelityFX FSR 1.0 is offered under the MIT License, whose notice heads each pass
file. AMD's source files themselves are not included.

- Project: https://github.com/GPUOpen-Effects/FidelityFX-FSR
- License: https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/master/license.txt (MIT)

## ScaleFX (built-in `scalefx` screen shader)

### 한국어

내장 화면 shader `scalefx`(issue #90)는 Sp00kyFox의 ScaleFX(2017-03-01) pass 다섯 개를 libretro `glsl-shaders`의
`edge-smoothing/scalefx`에서 **그대로** 가져온 것이고(`src/engine/post_shaders/scalefx/scalefx-pass*.glsl`), preset
파일만 rePIU가 같은 값으로 다시 썼습니다. ScaleFX는 MIT License로 제공되며, 각 pass 파일 머리에 고지 전문을 둡니다.

- Upstream: https://github.com/libretro/glsl-shaders/tree/master/edge-smoothing/scalefx
- License: 각 pass 파일의 머리말 (MIT, Copyright (c) 2016 Sp00kyFox)

### English

The built-in screen shader `scalefx` (issue #90) is the five passes of Sp00kyFox's ScaleFX (2017-03-01) taken
**unchanged** from libretro `glsl-shaders`' `edge-smoothing/scalefx` (`src/engine/post_shaders/scalefx/scalefx-pass*.glsl`);
only the preset file was written anew by rePIU to the same values. ScaleFX is offered under the MIT License, whose
full text heads every pass file.

- Upstream: https://github.com/libretro/glsl-shaders/tree/master/edge-smoothing/scalefx
- License: the header of each pass file (MIT, Copyright (c) 2016 Sp00kyFox)

## Anime4K v3.2 (built-in `anime4k` screen shader)

### 한국어

내장 화면 shader `anime4k`(issue #90)는 bloc97의 Anime4K v3.2 `Anime4K_Upscale_CNN_x2_S.glsl`(mpv 훅 파일)을
libretro 형식의 pass 다섯 개로 변환한 것입니다(`src/engine/post_shaders/anime4k/`, re2DJ #27의 파일). 신경망 가중치는
원본 그대로이고, 표본 좌표의 y 방향만 뒤집었습니다. Anime4K는 MIT License로 제공되며, 각 pass 파일 머리에 고지 전문을
둡니다. 실행 파일에 포함되며 게임 로직과는 무관한 표시 단계 후처리입니다.

- Project: https://github.com/bloc97/Anime4K
- License: https://github.com/bloc97/Anime4K/blob/master/LICENSE (MIT)

### English

The built-in screen shader `anime4k` (issue #90) is bloc97's Anime4K v3.2 `Anime4K_Upscale_CNN_x2_S.glsl` (an mpv
hook file) converted into five libretro-style passes (`src/engine/post_shaders/anime4k/`, the files of re2DJ #27).
The network's weights are the original's; only the sample coordinates' y direction is turned over. Anime4K is
offered under the MIT License, whose full text heads every pass file. It is embedded in the executables as a
presentation-only post-process unrelated to the game logic.

- Project: https://github.com/bloc97/Anime4K
- License: https://github.com/bloc97/Anime4K/blob/master/LICENSE (MIT)

## Galmuri 2.40.3 (project site font)

### 한국어

프로젝트 사이트(`docs/sites/`)는 한글을 포함한 픽셀 폰트 Galmuri 2.40.3의 `Galmuri14`, `Galmuri11-Bold`,
`GalmuriMono11` woff2 파일을 `docs/sites/static/fonts/`에 포함합니다. Galmuri는 SIL Open Font License 1.1로
제공되며, 라이선스 전문을 같은 디렉터리의 `OFL.txt`로 함께 배포합니다. 폰트는 수정하지 않았습니다. 사이트에만
쓰이며 rePIU 실행 파일에는 포함되지 않습니다.

- Project: https://github.com/quiple/galmuri
- License: [`docs/sites/static/fonts/OFL.txt`](docs/sites/static/fonts/OFL.txt) (SIL OFL 1.1, Reserved Font Name "Galmuri")

### English

The project site (`docs/sites/`) ships the `Galmuri14`, `Galmuri11-Bold` and `GalmuriMono11` woff2 files of
Galmuri 2.40.3, a pixel font with Hangul, in `docs/sites/static/fonts/`. Galmuri is offered under the SIL
Open Font License 1.1, whose full text is distributed alongside as `OFL.txt`. The fonts are unmodified.
They are used by the site only and are not part of the rePIU executables.

## Project site build tools

The site build (`scripts/site/`) installs these Python packages at build time; none is vendored or shipped
in the rePIU executables. Pinned in [`scripts/site/requirements.txt`](scripts/site/requirements.txt).

- markdown-it-py 4.2.0, mdit-py-plugins 0.6.1, mdurl 0.1.2 — MIT — https://github.com/executablebooks/markdown-it-py
- Jinja2 3.1.6, MarkupSafe 3.0.3 — BSD 3-Clause — https://github.com/pallets/jinja
- Mermaid 11.17.2 (loaded from jsDelivr by site pages that contain a diagram) — MIT — https://github.com/mermaid-js/mermaid
