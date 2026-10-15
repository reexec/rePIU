# 작업 지시: 브라우저 호스트 단계 4 단위 1 — Worker의 엔진이 WebGL2 캔버스에 그리기 (issue #72)

설계: [20261016-i072](../design/20261016-i072-browser-page.md)

브랜치: `task/i072-browser-page`. 빌드 트리: Win32 `build/`, 웹 `~/repiu-web`(WSL, emsdk).

## 순서

1. **`GlideHostWindow` 경계:** 인터페이스와 SDL 구현. 백엔드의 SDL 창·문맥·스왑·크기·전체 화면·제목·스왑 간격 호출을 옮김.
   Win32에서 렌더 probe 화소 일치·후처리·레터박스·전체 화면으로 회귀 확인. 이 단계만 먼저 커밋.
2. **플랫폼 함수:** `browser_canvas.h`(Win32·Linux "없음", 웹 EM_JS), `host_event_loop.h`(Win32·Linux 아무것도 안 함, 웹
   Asyncify + MessageChannel).
3. **브라우저 구현:** `GlideHostWindow`의 브라우저 캔버스 구현과 실행 중 선택.
4. **양보:** `OneThreadHostLoop::TakeTurn`에서 화면 교체 또는 16 ms마다.
5. **링크 타깃 `repiu_browser`:** 설계 결정 4의 옵션, `--pre-js`는 `window`가 없는 모든 환경에서 SDL dummy.
6. **페이지:** `src/platform/web/page/`(`index.html`, `main.js`, `worker.js`), 빌드 스크립트가 복사.
7. **검증:** 설계의 검증 1~4. 브라우저 확인은 사용자와 함께(Chrome).
8. **문서:** 작업 로그, `ARCHITECTURE.md`, `src/platform/web/README.md`, `docs/analysis/web-port-frontier.md`, `docs/TODO.md`,
   필요하면 `docs/kb/`(Asyncify·WORKERFS·OffscreenCanvas).

## 완료 기준

* 로컬 서버의 페이지에서 ZIP·CHD를 고르면 pumpitea 타이틀·데모가 캔버스에 나온다.
* Win32·Linux·Node 회귀 없음.

---

# Work order: browser host phase 4 unit 1 — the engine in a Worker drawing to a WebGL2 canvas (issue #72)

Design: [20261016-i072](../design/20261016-i072-browser-page.md)

Branch: `task/i072-browser-page`. Build trees: Win32 `build/`, web `~/repiu-web` (WSL, emsdk).

## Order

1. **The `GlideHostWindow` boundary:** the interface and the SDL implementation; the backend's SDL window, context, swap,
   size, fullscreen, title and swap-interval calls move there. On Win32, the render probe (pixel match, post-process,
   letterbox, fullscreen) checks for regressions. This step is committed on its own first.
2. **Platform functions:** `browser_canvas.h` (Win32 and Linux "none", web EM_JS), `host_event_loop.h` (nothing on Win32
   and Linux, Asyncify plus a MessageChannel on the web).
3. **Browser implementation:** the browser-canvas `GlideHostWindow` and the choice at run time.
4. **Yield:** in `OneThreadHostLoop::TakeTurn`, after a swap or every 16 ms.
5. **Link target `repiu_browser`:** decision 4's options; the `--pre-js` makes SDL's dummy drivers the default wherever
   there is no `window`.
6. **Page:** `src/platform/web/page/` (`index.html`, `main.js`, `worker.js`), copied by the build script.
7. **Verification:** the design's checks 1 to 4; the browser check with the user (Chrome).
8. **Documents:** the work log, `ARCHITECTURE.md`, `src/platform/web/README.md`, `docs/analysis/web-port-frontier.md`,
   `docs/TODO.md`, and `docs/kb/` if needed (Asyncify, WORKERFS, OffscreenCanvas).

## Done when

* Picking the ZIP and the CHD on the local server's page puts pumpitea's title and demo on the canvas.
* No Win32, Linux or Node regression.
