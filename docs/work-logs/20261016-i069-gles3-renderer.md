# 작업 로그: 브라우저 호스트 단계 3 — Glide 렌더러를 GLES 3 / WebGL2 부분집합으로 (issue #69)

설계: [20261016-i069](../design/20261016-i069-gles3-renderer.md) ·
작업 지시: [20261016-i069](../work-orders/20261016-i069-gles3-renderer.md)

## 2026-10-16 — 구현과 검증

브랜치 `task/i069-gles3-renderer`(main `942f60b`, v0.0.221에서). 결정 두 가지는 시작할 때 사용자가 정했습니다: 네이티브도
GL 3.3 core만(호환 경로 없음), 게임은 오프스크린 front/back FBO 쌍에 그림(단계 4가 아니라 이번에).

### 1. 기준 이미지 먼저

`repiu_glide_render_probe --opengl-scene <출력> [<기준>]`을 **포팅 전 렌더러에 먼저 커밋**했습니다(`320fe60`). 백엔드의 Glide
API만으로 장면을 그리므로 같은 코드가 포팅 전후에 그대로 돕니다. 장면에 넣은 것: 결합 함수(LOCAL, SCALE_OTHER,
SCALE_OTHER_ADD_LOCAL, BLEND, 반전), 상수색, RGB565 체커(반복·최근접, 원근, 고정·선형), ARGB4444 알파 램프에 `GREATER`
기준 `0x88`(경계 열 8은 떨어지고 9~15만 남는지), 반투명·가산 블렌드, 깊이 `LESS`(세 삼각형 겹침), 표 안개(w 1~4096), 점·선,
볼록 다각형, 일괄 그리기, 컬링, 색 마스크 끔, LFB 블릿 두 방향. 원점 위·아래 두 창 원점으로 그립니다.

포팅 전 렌더러로 두 번 그려 화소가 같음을 확인했고(결정적), 그 결과를 기준으로 보관했습니다.

### 2. 구현

| 부분 | 내용 |
|---|---|
| `GlApi` (`gl_api.*`) | GL 1.1을 넘는 함수 43개를 한 표로. 셰이더·후처리·백엔드가 따로 갖던 표를 대신함. `glClearDepthf`가 없으면 `glClearDepth` |
| 문맥 | 플랫폼의 `RequestedGlContext()`: Win32·Linux `{core, 3.3}`, 웹 `{ES, 3.0}`. 창 framebuffer의 깊이 비트는 0(깊이는 FBO 쌍) |
| GLSL 방언 (`gl_shader_dialect.*`) | `#version 330 core` / `#version 300 es`+`precision highp`. 후처리 shader는 단어 치환(`attribute`, `varying`, `texture2D`, `gl_FragColor`). 매크로 대신 치환을 고른 것은 `gl_` 예약 이름을 `#define`하지 않기 위해서 |
| 결합 셰이더 | 속성 3개(공용 slot 0·1·2), uniform 투영·점 크기, 알파 테스트 `discard`(clamp한 최종 알파). 결합·안개 식은 그대로 옮김(스크립트로 원문에서 잘라 붙임) |
| `GlideVertexStream` | 4 MB 스트리밍 VBO + VAO. 그리기마다 `glBufferSubData` → `glDrawArrays`, 끝에서 orphan |
| `GlideFramebufferPair` | 그림 사각형 크기의 RGBA8 FBO 두 장 + 공용 depth24. `Exchange`, `ReadBack`, `Present`(창을 검게 지우고 front를 사각형에, 후처리면 그 shader로) |
| `GlUnitQuad`, `BuildGlProgram` | 표시·후처리의 단위 사각형, 공용 slot으로 program 만들기 |
| 후처리 | 입력이 back buffer 복사본이 아니라 front 텍스처. `glPushAttrib` 없음 |
| OSD | ImGui에 문맥의 머리말 문자열 |
| `SetRenderBuffer(front)` | front FBO를 그리기 대상으로. 첫 선택에 한 줄 |
| 웹 | `legacy_gl_stubs.cpp` 삭제, `-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sGL_ENABLE_GET_PROC_ADDRESS=1` |

### 3. 기준 이미지 대조

Windows Release, NVIDIA RTX 4090(드라이버 616.56). 문맥은 `3.3.0 NVIDIA`(core)로 만들어졌습니다.

| 장면 (320×240, 창 2배) | 다른 화소 | 최대 차이 |
|---|---|---|
| upper_left | 0 / 76,800 | 0 |
| lower_left | 0 / 76,800 | 0 |
| lfb | 0 / 76,800 | 0 |
| lfb_flipped | 0 / 76,800 | 0 |

FNV-1a 해시까지 포팅 전과 같습니다. 셰이더 알파 테스트의 경계(`GREATER`, 알파 = 기준)도 고정 기능과 같게 떨어졌습니다.
기존 모드 `--opengl-post-shader`(scanline 줄무늬, crt 곡률, `none`, 레터박스 사각형, 전체 화면 왕복, swap 뒤 상태 복원)와
`--opengl-lfb`, 인자 없는 모드도 통과합니다. core probe 통과(`core_probe_all=true`).

### 4. 처리량 — 첫 판은 절반으로 떨어졌다

pumpit8 attract, vsync OFF(`REPIU_GLIDE_SWAP_INTERVAL=0`), `REPIU_GLIDE_FRAME_RATE_LOG`, `REPIU_GLIDE_SWAP_TIME_PROFILE`.

| 빌드 | 후처리 | fps (안정 구간) | swap setup 사이클 / 프레임 | present 사이클 / 프레임 |
|---|---|---|---|---|
| 포팅 전 | 없음 | 약 1,090 | 약 1.5k | 약 273k |
| 첫 판 | 없음 | 약 570 | 약 195k | 약 2.3M |
| **고친 뒤** | 없음 | **약 1,100** | 약 5.7k | **약 136k** |
| 포팅 전 | crt | 약 1,000 | | |
| 첫 판 | crt | 약 570 | | |
| **고친 뒤** | crt | **약 1,000~1,040** | | |

첫 판은 표시와 LFB 블릿에서 게임 상태를 `glGet*`/`glIsEnabled` 스냅샷(15회)으로 저장·복원했습니다. NVIDIA 드라이버의 스레드
최적화에서 조회는 드라이버 스레드가 밀린 명령을 다 처리할 때까지 기다리게 하므로, setup 비용이 약 130배, 스왑 비용이
약 8배가 됐습니다. 설계 결정 8의 원래 뜻대로 **setter가 기록한 게임 상태(`GameGlState`)로 되돌리도록** 바꾸자 포팅 전과
같아졌고, present 비용은 오히려 절반입니다(창 framebuffer에 사각형 하나만 그림). `GlideFramebufferPair::ReadBack`도 읽기 바인딩을
조회하지 않고 그리기 대상으로 되돌립니다. 스냅샷은 창 크기가 바뀔 때(`Resize`)만 씁니다.

이 수정에 맞춰 probe의 상태 복원 검사도 GL을 직접 켜는 대신 Glide API(`SetAlphaBlend`)로 블렌드를 켜고 swap 뒤를 봅니다.

### 5. 게임 화면

`REPIU_GLIDE_PIXEL_DIAG_INTERVAL_MS=5000`과 PrintWindow 캡처로 포팅 전후를 번갈아 돌렸습니다(동시 실행 없음).

* pumpitea: 5초 표본의 밝기 격자가 같고(`0000000000000266`, 평균색 196,147,78), 타이틀(EXTRA 로고)·데모 플레이 화면이
  정상입니다. 그 뒤 표본은 움직이는 입자와 무작위 데모 곡 때문에 프레임이 달라 직접 비교할 수 없습니다. 스왑 속도는 둘 다
  초당 약 60.
* pumpit8 + crt: 같은 데모 장면에서 곡률·마스크·비네트가 포팅 전과 같은 모양입니다.

### 6. 다른 빌드

| 구성 | 결과 |
|---|---|
| Win32 `direct` Debug·Release | 빌드, 장면 probe 4개 화소 일치, `--opengl-post-shader`·`--opengl-lfb` 통과, core probe 통과 |
| Win32 `emulated` Release (단일 스레드 루프) | 빌드, core probe 통과, pumpitea 60초 정상 정지(문맥 `3.3.0 NVIDIA`, 스왑 504회) |
| Linux x64 Release (WSL) | 빌드, core probe 통과, WSLg에서 pumpitea 40초: `4.6 (Core Profile) Mesa`, D3D12(RTX 4090), 30초 표본에 타이틀 화면 |
| Linux i386 Release (WSL) | 빌드, core probe 통과, WSLg에서 pumpitea 35초: `4.5 (Core Profile) Mesa`, llvmpipe(i386용 D3D12 드라이버가 없는 기존 상황), 30초 표본에 타이틀 화면 |
| 웹 (Emscripten 6.0.8, `~/repiu-web`) | `legacy_gl_stubs.cpp` 없이 `repiu`·`repiu_core_probe` 링크(정의 안 된 심볼 없음), Node core probe 통과(#67과 같은 12개 건너뜀), Node에서 pumpitea 30초 `stop requested`로 정상 정지(Glide dummy) |

마지막 수정(점 크기 범위를 문맥 생성 때 한 번만 읽음)은 Linux·웹 빌드 뒤에 들어갔고, Win32 Debug·Release에서만 다시
확인했습니다. 플랫폼 분기가 없는 엔진 코드 세 줄입니다.

### 남은 것

* ~~사용자 플레이 확인(Windows)~~ — 2026-10-16 사용자 확인: 정상.
* 단계 4: 브라우저 페이지(Worker, OffscreenCanvas의 WebGL2 문맥은 `antialias:false`, `depth:false`면 됨 — 깊이는 FBO에 있음).

---

# Work log: browser host phase 3 — the Glide renderer on the GLES 3 / WebGL2 subset (issue #69)

Design: [20261016-i069](../design/20261016-i069-gles3-renderer.md) ·
Work order: [20261016-i069](../work-orders/20261016-i069-gles3-renderer.md)

## 2026-10-16 — implemented and verified

Branch `task/i069-gles3-renderer` (main `942f60b`, from v0.0.221). The user settled two decisions at the start: native
builds use GL 3.3 core only (no compatibility path), and the game draws into an offscreen front/back pair of FBOs (in this
phase rather than phase 4).

### 1. The reference image first

`repiu_glide_render_probe --opengl-scene <output> [<reference>]` was **committed against the renderer before the port**
(`320fe60`). It draws through the backend's Glide API only, so the same code runs before and after. The scene holds: the
combine functions (LOCAL, SCALE_OTHER, SCALE_OTHER_ADD_LOCAL, BLEND, inversion), the constant color, an RGB565 checker
(repeat with nearest, perspective, clamp with linear), an ARGB4444 alpha ramp under `GREATER` against `0x88` (boundary
column 8 must drop, 9 to 15 stay), translucent and additive blending, depth `LESS` (three overlapping triangles), table fog
(w from 1 to 4096), points and lines, a convex polygon, a batch, culling, the color mask off, and the LFB blit both ways,
in both window origins.

Drawn twice on the old renderer the pixels matched (deterministic), and that output was kept as the reference.

### 2. Implementation

| Part | Content |
|---|---|
| `GlApi` (`gl_api.*`) | the 43 functions beyond GL 1.1 in one table, replacing the ones the shader, post-process and backend each kept; `glClearDepth` when `glClearDepthf` is absent |
| Context | the platform's `RequestedGlContext()`: Win32 and Linux `{core, 3.3}`, web `{ES, 3.0}`; the window framebuffer has no depth bits (depth lives with the FBO pair) |
| GLSL dialect (`gl_shader_dialect.*`) | `#version 330 core` / `#version 300 es` + `precision highp`; post-process shaders get whole-word substitution (`attribute`, `varying`, `texture2D`, `gl_FragColor`), chosen over macros so as not to `#define` reserved `gl_` names |
| Combine shader | three attributes (shared slots 0, 1, 2), a uniform projection and point size, an alpha-test `discard` on the clamped final alpha; the combine and fog expressions carried over (cut from the original by script) |
| `GlideVertexStream` | a 4 MB streaming VBO plus a VAO; each draw `glBufferSubData` then `glDrawArrays`, orphaned at the end |
| `GlideFramebufferPair` | two RGBA8 FBOs the picture rectangle's size plus a shared depth24; `Exchange`, `ReadBack`, `Present` (the window cleared black, front drawn into the rectangle, through the post-process shader when one is active) |
| `GlUnitQuad`, `BuildGlProgram` | the unit quad of presentation and post-processing, programs built on the shared slots |
| Post-process | reads the front texture instead of a copy of the back buffer; no `glPushAttrib` |
| OSD | ImGui gets the context's header string |
| `SetRenderBuffer(front)` | binds the front FBO as the draw target; one line on the first selection |
| Web | `legacy_gl_stubs.cpp` deleted; `-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sGL_ENABLE_GET_PROC_ADDRESS=1` |

### 3. Against the reference

Windows Release, NVIDIA RTX 4090 (driver 616.56); the context came up as `3.3.0 NVIDIA` (core).

| Scene (320×240, window 2×) | Differing pixels | Largest delta |
|---|---|---|
| upper_left | 0 / 76,800 | 0 |
| lower_left | 0 / 76,800 | 0 |
| lfb | 0 / 76,800 | 0 |
| lfb_flipped | 0 / 76,800 | 0 |

Even the FNV-1a hashes match the pre-port ones. The shader alpha test's boundary (`GREATER` with alpha equal to the
reference) falls the same way as fixed function. The existing modes `--opengl-post-shader` (scanline rows, crt curvature,
`none`, the letterbox rectangle, the fullscreen round trip, state restored after a swap), `--opengl-lfb` and the bare mode
pass too. The core probe passes (`core_probe_all=true`).

### 4. Throughput — the first cut halved it

pumpit8 attract, vsync OFF (`REPIU_GLIDE_SWAP_INTERVAL=0`), `REPIU_GLIDE_FRAME_RATE_LOG`, `REPIU_GLIDE_SWAP_TIME_PROFILE`.

| Build | Post-process | fps (steady) | swap setup cycles / frame | present cycles / frame |
|---|---|---|---|---|
| before the port | none | about 1,090 | about 1.5k | about 273k |
| first cut | none | about 570 | about 195k | about 2.3M |
| **fixed** | none | **about 1,100** | about 5.7k | **about 136k** |
| before the port | crt | about 1,000 | | |
| first cut | crt | about 570 | | |
| **fixed** | crt | **about 1,000 to 1,040** | | |

The first cut saved and restored the game's state around the present and the LFB blit with a `glGet*`/`glIsEnabled`
snapshot (15 queries). Under the NVIDIA driver's threaded optimization a query waits for the driver thread to work through
its backlog, so setup cost about 130 times as much and the swap about 8 times. Putting the state back from **what the
setters recorded (`GameGlState`)**, which is what design decision 8 meant, brought it back to the pre-port level, and the
present now costs half as much (one quad into the window's framebuffer). `GlideFramebufferPair::ReadBack` no longer
queries the read binding either; it binds the draw target back. The snapshot remains only for resizing (`Resize`).

With that change the probe's state check enables blending through the Glide API (`SetAlphaBlend`) rather than GL itself,
and looks after the swap.

### 5. The game's picture

Before and after were run alternately (never at the same time) with `REPIU_GLIDE_PIXEL_DIAG_INTERVAL_MS=5000` and
PrintWindow captures.

* pumpitea: the 5 s sample's brightness grid matches (`0000000000000266`, mean color 196,147,78), and the title (the
  EXTRA logo) and the demo play look right. Later samples cannot be compared directly: moving particles and a random demo
  song make them different frames. Both swap about 60 times a second.
* pumpit8 + crt: on the same demo scene the curvature, mask and vignette look as before the port.

### 6. The other builds

| Configuration | Result |
|---|---|
| Win32 `direct` Debug and Release | build, the four probe scenes match pixel for pixel, `--opengl-post-shader` and `--opengl-lfb` pass, the core probe passes |
| Win32 `emulated` Release (the single-threaded loop) | build, the core probe passes, pumpitea stops cleanly after 60 s (context `3.3.0 NVIDIA`, 504 swaps) |
| Linux x64 Release (WSL) | build, the core probe passes, pumpitea for 40 s under WSLg: `4.6 (Core Profile) Mesa`, D3D12 (RTX 4090), the title on the 30 s sample |
| Linux i386 Release (WSL) | build, the core probe passes, pumpitea for 35 s under WSLg: `4.5 (Core Profile) Mesa`, llvmpipe (no i386 D3D12 driver, as before), the title on the 30 s sample |
| Web (Emscripten 6.0.8, `~/repiu-web`) | `repiu` and `repiu_core_probe` link without `legacy_gl_stubs.cpp` (no undefined symbols), the Node core probe passes (the same 12 skipped as in #67), pumpitea under Node stops cleanly with `stop requested` after 30 s (Glide dummy) |

The last change (the point-size range read once when the context is made) came after the Linux and web builds and was
rechecked on Win32 Debug and Release only; it is three lines of engine code with no platform branch.

### Remaining

* ~~A play check by the user (Windows)~~ — confirmed by the user on 2026-10-16: it works.
* Phase 4: the browser page (the OffscreenCanvas WebGL2 context can be `antialias:false`, `depth:false`: depth lives in
  the FBOs).
