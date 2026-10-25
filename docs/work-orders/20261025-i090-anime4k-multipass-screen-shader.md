# 작업 지시: 다단계 화면 shader(`.glslp`)와 내장 업스케일러 `fsr`·`scalefx`·`anime4k` (issue #90)

설계: [20261025-i090](../design/20261025-i090-anime4k-multipass-screen-shader.md)

1. `post_shader_preset.{h,cpp}`: `.glslp` 부분집합 파서, 단일 pass, pass 크기 (GL 없음).
2. 카탈로그를 내장 파일 묶음과 `LoadPostShaderPreset`으로 바꾸고, CMake가 목록의 파일을 12000바이트 조각으로 생성
   헤더에 넣게 한다. 목록에 `.glslp`.
3. `GlidePostProcess`: pass별 프로그램·FBO 대상, `Apply`가 체인을 그리고 실패하면 거짓을 돌려줌, stock pass.
   `GlideFramebufferPair::Present`는 거짓이면 평범한 그리기. `AdaptLegacyGlslStage`는 자기 출력을 선언한 1.30
   스타일 본문에 출력을 더하지 않음.
4. 내장 `fsr/`·`scalefx/`·`anime4k/`(re2DJ #26·#27의 파일과 preset), 명령행 도움말.
5. probe: `post_shader_probe`에 preset 파서와 내장 다섯·`.glslp` 목록, `glide_render_probe`에 세 체인의 실제 GL 검사.
6. 문서: 가이드, kb, README, ARCHITECTURE, THIRD_PARTY_NOTICES, 작업 로그.
7. 검증: Win32 Debug·Release 빌드와 두 probe, WSL Linux x64 빌드, 웹 빌드(가능하면), 실제 실행 로그.

---

# Work order: multi-pass screen shaders (`.glslp`) and the built-in upscalers `fsr`, `scalefx` and `anime4k` (issue #90)

Design: [20261025-i090](../design/20261025-i090-anime4k-multipass-screen-shader.md)

1. `post_shader_preset.{h,cpp}`: the `.glslp` subset parser, the single pass and pass sizes (no GL).
2. Turn the catalog into embedded file sets with `LoadPostShaderPreset`, and have CMake embed the listed files in
   12000-byte pieces in the generated header. `.glslp` in the listing.
3. `GlidePostProcess`: per-pass programs and framebuffer targets, `Apply` drawing the chain and returning false on
   failure, the stock pass. `GlideFramebufferPair::Present` draws plainly on false. `AdaptLegacyGlslStage` adds no
   output to a 1.30-style body that declares its own.
4. The built-in `fsr/`, `scalefx/` and `anime4k/` (re2DJ #26 and #27's files and presets), the command-line help.
5. Probes: the preset parser, the five built-ins and the `.glslp` listing in `post_shader_probe`; the three chains
   in a real GL in `glide_render_probe`.
6. Documents: the guide, kb, README, ARCHITECTURE, THIRD_PARTY_NOTICES, the work log.
7. Verification: Win32 Debug and Release builds with both probes, the WSL Linux x64 build, the web build when
   possible, a real run's log.
