# 작업 로그: 다단계 화면 shader(`.glslp`)와 내장 업스케일러 `fsr`·`scalefx`·`anime4k` (issue #90)

설계: [20261025-i090](../design/20261025-i090-anime4k-multipass-screen-shader.md) ·
작업 지시: [20261025-i090](../work-orders/20261025-i090-anime4k-multipass-screen-shader.md)

## 2026-10-25 — 구현과 확인

브랜치 `task/i090-anime4k-screen-shader`. 처음 요청은 re2DJ #27의 `anime4k`였고, 작업 중 사용자 요청으로 같은
변경의 `fsr`·`scalefx`(re2DJ #26·#27)도 범위에 넣었습니다.

### 구현

| 파일 | 내용 |
|---|---|
| `post_shader_preset.{h,cpp}` (새) | libretro `.glslp` 부분집합 파서(`shaders`, `shaderN`, `scale_typeN`/`_x`/`_y`, `scaleN`/`_x`/`_y`, `filter_linearN`, `float_framebufferN`), `SinglePassPreset`, `PostShaderPassSize`. re2DJ의 것을 옮김 |
| `post_shader_catalog.{h,cpp}`, `post_shader_builtins.inc.in`, `CMakeLists.txt` | 내장을 포함된 파일 묶음(`kPostShaderBuiltinFiles`, 15개 파일을 12000바이트 조각으로)과 시작 파일로. `LoadPostShaderText` → `LoadPostShaderPreset`(내장 묶음 안의 상대 경로 접기, 파일 preset은 옆에서 읽음). 목록에 `.glslp` |
| `glide_post_process.{h,cpp}` | pass별 프로그램·uniform·FBO 대상(`RGBA8`/`RGBA16F`), `Apply`가 체인 전부를 그림(unit 0 `Texture`, 1 `OrigTexture`, 2+ `PassPrevN`), 배율 있는 마지막 pass는 stock pass, 대상 실패 시 거짓 반환과 shader 내림, `pass_count()`, 로그에 pass 수 |
| `glide_framebuffer_pair.cpp` | `Apply`가 거짓이면 평범한 그리기 |
| `gl_shader_dialect.{h,cpp}` | 본문이 `out … vec4 …;`를 스스로 선언하면(`#version 130` 스타일) `repiuFragColor` 출력을 더하지 않음 |
| `src/engine/post_shaders/fsr/`, `scalefx/`, `anime4k/` (새) | re2DJ의 파일 그대로(머리말의 프로젝트 이름만 바꿈; ScaleFX pass 파일은 libretro 원본과 바이트 단위로 같음) |
| `command_line_options.cpp` | 도움말의 내장 목록 |
| `post_shader_probe.cpp`, `glide_render_probe/main.cpp` | preset 파서 네 묶음, 내장 다섯과 `.glslp` 목록·깨진 preset, 실제 GL에서 세 체인 |
| 문서 | 가이드(업스케일러 표, preset 절, 확인 6), kb(6·7절), README, ARCHITECTURE, THIRD_PARTY_NOTICES(FSR·ScaleFX·Anime4K) |

### 확인

(아래에 채움)

### 하지 못한 것

* WSL(Ubuntu 24.04)에 cmake·gcc·emsdk가 없어 Linux x64와 웹 빌드는 이 PC에서 돌리지 못했습니다. 코드는 GL 3.3
  core / ES 3.0 공용 부분집합만 쓰고(`GlApi`의 FBO·텍스처 호출, `AdaptLegacyGlslStage`), 플랫폼 분기가 없습니다.
  브라우저에서 세 preset의 ES 컴파일과 `RGBA16F` 대상 완성은 다음 웹 빌드에서 확인할 항목입니다.

---

# Work log: multi-pass screen shaders (`.glslp`) and the built-in upscalers `fsr`, `scalefx` and `anime4k` (issue #90)

Design: [20261025-i090](../design/20261025-i090-anime4k-multipass-screen-shader.md) ·
Work order: [20261025-i090](../work-orders/20261025-i090-anime4k-multipass-screen-shader.md)

## 2026-10-25 — implementation and checks

Branch `task/i090-anime4k-screen-shader`. The request began as re2DJ #27's `anime4k`; during the work the user
asked for `fsr` and `scalefx` of the same change (re2DJ #26 and #27) in the same scope.

### Implementation

| File | What |
|---|---|
| `post_shader_preset.{h,cpp}` (new) | The libretro `.glslp` subset parser (`shaders`, `shaderN`, `scale_typeN`/`_x`/`_y`, `scaleN`/`_x`/`_y`, `filter_linearN`, `float_framebufferN`), `SinglePassPreset`, `PostShaderPassSize`; re2DJ's, carried over |
| `post_shader_catalog.{h,cpp}`, `post_shader_builtins.inc.in`, `CMakeLists.txt` | Built-ins as embedded file sets (`kPostShaderBuiltinFiles`, 15 files in 12000-byte pieces) with a main file. `LoadPostShaderText` → `LoadPostShaderPreset` (relative paths folded inside the embedded set, a file preset read beside itself). `.glslp` in listings |
| `glide_post_process.{h,cpp}` | Per-pass programs, uniforms and framebuffer targets (`RGBA8`/`RGBA16F`); `Apply` draws the whole chain (unit 0 `Texture`, 1 `OrigTexture`, 2+ `PassPrevN`), a scaled last pass through the stock pass, false and the shader dropped when a target fails; `pass_count()`; the pass count in the log line |
| `glide_framebuffer_pair.cpp` | The plain draw when `Apply` returns false |
| `gl_shader_dialect.{h,cpp}` | No `repiuFragColor` output added when the body declares `out … vec4 …;` itself (the `#version 130` style) |
| `src/engine/post_shaders/fsr/`, `scalefx/`, `anime4k/` (new) | re2DJ's files (only the project name in the headers changed; the ScaleFX pass files are byte-identical to libretro's) |
| `command_line_options.cpp` | The built-in list in the help |
| `post_shader_probe.cpp`, `glide_render_probe/main.cpp` | Four preset-parser groups, the five built-ins, the `.glslp` listing and a broken preset; the three chains in a real GL |
| Documents | The guide (upscaler table, preset section, check 6), kb (sections 6 and 7), README, ARCHITECTURE, THIRD_PARTY_NOTICES (FSR, ScaleFX, Anime4K) |

### Checks

(filled in below)

### Not done

* WSL (Ubuntu 24.04) has no cmake, gcc or emsdk, so the Linux x64 and web builds could not be run on this PC. The
  code uses only the shared GL 3.3 core / ES 3.0 subset (`GlApi`'s framebuffer and texture calls,
  `AdaptLegacyGlslStage`) and has no platform branch. The ES compile of the three presets and the completeness of
  `RGBA16F` targets in a browser are items for the next web build.
