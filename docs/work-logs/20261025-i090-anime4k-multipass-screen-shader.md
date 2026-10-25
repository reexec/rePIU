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

2026-10-26, 다른 PC에서:

* Win32 Debug·Release 빌드: 오류 없음, 바뀐 파일에서 경고 없음. 이 트리는 처음에 생성 헤더
  `post_shader_builtins.inc`가 10-05 것으로 남아 `kPostShaderBuiltinFiles`가 없다는 오류가 났고, `cmake build`로
  다시 구성하자 17개 파일이 든 헤더가 생겨 해결됐습니다.
* `repiu_aot_probe --post-shader`(Debug·Release): `post_shader_assembly/parameters/builtins/preset/catalog/variable`
  모두 `true`, `post_shader_all=true`.
* `repiu_glide_render_probe --opengl-post-shader`(Debug·Release, 실제 GL): `crt (1 passes, 6 parameters)`,
  `fsr (2 passes, 1 parameters)`, `scalefx (5 passes, 3 parameters)`, `anime4k (5 passes, 0 parameters)`;
  `post_shader_gl_fsr/scalefx/anime4k=true bright=12288/12288`, `scanline`·`crt`·`none`·`state_restored`·`letterbox`·
  전체 화면 전환 모두 `true`, `glide_render_probe=pass`. 인자 없는 실행도 `pass`.
* Linux x64 Debug(WSL Ubuntu 24.04, `scripts/build_linux_x64.sh --build-dir build/linux_x64_debug`): 빌드 성공,
  경고 2개는 이번 변경과 무관한 파일(`execution_trampoline.cpp`, `fault_handler_arch.cpp`). 두 probe는
  `if(WIN32)` 안의 대상이라 Linux에서는 컴파일까지만 확인됩니다.
* 가이드 확인 1의 `crt` 로그 줄을 실제 형식 `(1 passes, 6 parameters)`로 고쳤습니다.
* 실제 실행: 사용자가 게임에서 내장 업스케일러가 잘 동작하는 것을 확인했습니다(2026-10-26).

### 하지 못한 것

* 이 PC의 WSL에는 emsdk가 없어 웹 빌드는 돌리지 못했습니다. 코드는 GL 3.3 core / ES 3.0 공용 부분집합만
  쓰고(`GlApi`의 FBO·텍스처 호출, `AdaptLegacyGlslStage`), 플랫폼 분기가 없습니다. 브라우저에서 세 preset의 ES
  컴파일과 `RGBA16F` 대상 완성은 다음 웹 빌드에서 확인할 항목입니다.

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

2026-10-26, on another PC:

* Win32 Debug and Release builds: no errors, no warnings from the changed files. At first this tree still had the
  generated `post_shader_builtins.inc` of 10-05, so `kPostShaderBuiltinFiles` was missing; reconfiguring with
  `cmake build` wrote the header with its 17 files and the build went through.
* `repiu_aot_probe --post-shader` (Debug and Release): `post_shader_assembly/parameters/builtins/preset/catalog/variable`
  all `true`, `post_shader_all=true`.
* `repiu_glide_render_probe --opengl-post-shader` (Debug and Release, a real GL): `crt (1 passes, 6 parameters)`,
  `fsr (2 passes, 1 parameters)`, `scalefx (5 passes, 3 parameters)`, `anime4k (5 passes, 0 parameters)`;
  `post_shader_gl_fsr/scalefx/anime4k=true bright=12288/12288`, and `scanline`, `crt`, `none`, `state_restored`,
  `letterbox` and the fullscreen toggle all `true`; `glide_render_probe=pass`. The run with no argument passes too.
* Linux x64 Debug (WSL Ubuntu 24.04, `scripts/build_linux_x64.sh --build-dir build/linux_x64_debug`): builds; its two
  warnings are in files this change does not touch (`execution_trampoline.cpp`, `fault_handler_arch.cpp`). Both
  probes are targets inside `if(WIN32)`, so on Linux the change is checked as far as compiling.
* Check 1 of the guide now gives the `crt` log line in its real form, `(1 passes, 6 parameters)`.
* A real run: the user checked in the game that the built-in upscalers work (2026-10-26).

### Not done

* This PC's WSL has no emsdk, so the web build could not be run. The code uses only the shared GL 3.3 core / ES 3.0 subset (`GlApi`'s framebuffer and texture calls,
  `AdaptLegacyGlslStage`) and has no platform branch. The ES compile of the three presets and the completeness of
  `RGBA16F` targets in a browser are items for the next web build.
