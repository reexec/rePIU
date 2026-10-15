# 작업 지시: 브라우저 호스트 단계 3 — Glide 렌더러를 GLES 3 / WebGL2 부분집합으로 (issue #69)

설계: [20261016-i069](../design/20261016-i069-gles3-renderer.md)

브랜치: `task/i069-gles3-renderer`. 빌드 트리: Win32 `build/`(Release), Linux `build/linux_*`(WSL), 웹 `~/repiu-web`(WSL).

## 순서

1. **기준 이미지:** `repiu_glide_render_probe --opengl-scene <파일>`을 지금 렌더러(GL 2.1 호환)에 더해 커밋하고, Win32
   Release로 기준 RGBA를 떠서 scratchpad에 보관. 게임 화면 기준도 이때 캡처(pumpitea·pumpit8, vsync OFF fps 포함).
2. **공용 함수 표와 방언:** `gl_api`, `gl_shader_dialect`. 셰이더·후처리가 공용 표를 쓰게 바꿈(동작 그대로).
3. **문맥 요청:** `RequestedGlContext()`(플랫폼 3곳), `OpenWindowed`가 그 값으로 SDL 속성을 설정. ImGui 머리말.
4. **꼭짓점 스트림과 셰이더:** `GlideVertexStream`, 결합 셰이더를 속성·uniform 투영·알파 테스트 `discard`로. 즉시 모드 제거.
5. **프레임버퍼 쌍:** `GlideFramebufferPair`, `SetRenderBuffer`·`Clear`·`BufferSwap`·`ReadbackFramebuffer`·LFB 블릿·진단 읽기를
   FBO로. 표시 사각형과 후처리 `Apply`를 텍스처 입력으로. `glPushAttrib` 제거.
6. **남은 데스크톱 전용 호출 정리:** `GL_CLAMP`, `glClearDepth`, `GL_FOG`, `GL_RGB` 읽기, `GL_PROJECTION_MATRIX`.
7. **웹:** `legacy_gl_stubs.cpp` 삭제, 링크 옵션(`GL_ENABLE_GET_PROC_ADDRESS`, WebGL 2), 빌드와 Node 실행.
8. **검증:** 설계의 검증 1~6. 기준 이미지 비교 결과를 작업 로그에.
9. **문서:** 작업 로그, `ARCHITECTURE.md`(렌더러 구조), `docs/analysis/web-port-frontier.md`, `docs/TODO.md`, 필요하면
   `docs/kb/`(GLES 3 / WebGL2와 데스크톱 GL의 차이), `src/platform/web/README.md`, README의 최소 요구 사항(GL 3.3).

## 완료 기준

* Windows GL 3.3 core에서 장면 probe가 기준 이미지와 같거나, 차이가 설명된다. pumpitea·pumpit8 회귀 없음.
* Linux 빌드·probe 통과, 웹 빌드가 stub 없이 링크.

---

# Work order: browser host phase 3 — the Glide renderer on the GLES 3 / WebGL2 subset (issue #69)

Design: [20261016-i069](../design/20261016-i069-gles3-renderer.md)

Branch: `task/i069-gles3-renderer`. Build trees: Win32 `build/` (Release), Linux `build/linux_*` (WSL), web
`~/repiu-web` (WSL).

## Order

1. **Reference image:** add `repiu_glide_render_probe --opengl-scene <file>` to today's renderer (GL 2.1 compatibility)
   and commit it; capture the reference RGBA with Win32 Release and keep it in the scratchpad. Capture the game reference
   too (pumpitea and pumpit8, with vsync-OFF fps).
2. **Shared function table and dialect:** `gl_api`, `gl_shader_dialect`; the shader and post-process use the shared table
   (behaviour unchanged).
3. **Context request:** `RequestedGlContext()` (three platforms); `OpenWindowed` sets the SDL attributes from it. The
   ImGui header.
4. **Vertex stream and shader:** `GlideVertexStream`; the combine shader with attributes, a uniform projection and
   alpha-test `discard`. Immediate mode removed.
5. **Framebuffer pair:** `GlideFramebufferPair`; `SetRenderBuffer`, `Clear`, `BufferSwap`, `ReadbackFramebuffer`, the LFB
   blit and diagnostic reads move to the FBOs. The present quad and the post-process `Apply` take a texture input.
   `glPushAttrib` removed.
6. **Remaining desktop-only calls:** `GL_CLAMP`, `glClearDepth`, `GL_FOG`, `GL_RGB` reads, `GL_PROJECTION_MATRIX`.
7. **Web:** delete `legacy_gl_stubs.cpp`; link options (`GL_ENABLE_GET_PROC_ADDRESS`, WebGL 2); build and Node run.
8. **Verification:** the design's checks 1 to 6, with the reference-image comparison in the work log.
9. **Documents:** the work log, `ARCHITECTURE.md` (renderer structure), `docs/analysis/web-port-frontier.md`,
   `docs/TODO.md`, `docs/kb/` if needed (GLES 3 / WebGL2 against desktop GL), `src/platform/web/README.md`, and the
   README's minimum requirement (GL 3.3).

## Done when

* On Windows GL 3.3 core the scene probe matches the reference image, or the difference is explained. No pumpitea or
  pumpit8 regression.
* The Linux builds and probes pass, and the web build links without stubs.
