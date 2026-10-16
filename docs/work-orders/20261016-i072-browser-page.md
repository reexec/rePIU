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

## 후속: 롬 고르기 (2026-10-16, 설계 결정 9)

브랜치: `task/i072-rom-picker`.

1. `rom_selection.js`: 폴더의 파일 목록(`webkitRelativePath`)에서 게임 후보를 찾는 순수 함수, 각각 고른 ZIP·CHD를 묶는 함수.
   Node 시험(`rom_selection.test.js`): 롬 폴더 배치, 평평한 배치, 여러 게임, 짝 없는 ZIP·CHD, 대소문자.
2. `index.html`·`main.js`: 폴더 칸, ZIP 칸, CHD 칸, 게임 목록. 어느 쪽이든 고른 결과를 상태 줄에 보임.
3. `worker.js`: `{game, zips, chd}`를 받아 ZIP들과 CHD를 마운트.
4. CMake가 `rom_selection.js`도 복사. 웹 빌드로 페이지 파일이 나오는지 확인.
5. 작업 로그 #72에 후속 절.

완료 기준: 폴더 하나로도, ZIP·CHD를 각각으로도 pumpitea를 고를 수 있고 Node 시험이 통과한다. 브라우저 확인은 사용자.

## 후속: 진행이 보이는 페이지 (2026-10-16, 설계 결정 10)

1. `PreparePiuChdMount`에 풀기 관찰자(선택 인자). 디렉터리 기록으로 전체를 먼저 세고, 시작과 약 16 MB마다 알림.
2. 로더: 준비 시작 한 줄, 관찰자가 풀기 시작과 진행(%·파일·MB)을 로그로.
3. 페이지: 로그를 캔버스 옆에, 경과 시간·줄 수·마지막 줄, 따라가기 스크롤, 복사 버튼.
4. 확인: Win32 빌드(로더, `repiu_exe_analyzer`), Node에서 진행 줄, 헤드리스 브라우저에서 시작 직후부터 줄이 보이는지.

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

## Follow-up: picking the ROMs (2026-10-16, design decision 9)

Branch: `task/i072-rom-picker`.

1. `rom_selection.js`: a pure function finding game candidates in a folder's file list (`webkitRelativePath`), and one
   pairing a separately picked ZIP and CHD. A Node test (`rom_selection.test.js`): the ROM-folder layout, the flat
   layout, several games, an unpaired ZIP or CHD, letter case.
2. `index.html` and `main.js`: a folder field, a ZIP field, a CHD field and a game list; either way, the status line shows
   what was picked.
3. `worker.js`: receives `{game, zips, chd}` and mounts the ZIPs and the CHD.
4. CMake copies `rom_selection.js` too; check the web build produces the page files.
5. A follow-up section in work log #72.

Done when: pumpitea can be picked with one folder and with the ZIP and the CHD separately, and the Node test passes. The
browser check is the user's.

## Follow-up: a page that shows its progress (2026-10-16, design decision 10)

1. An unpack observer for `PreparePiuChdMount` (an optional argument): the total counted from the directory records
   first, then a report at the start and about every 16 MB.
2. Loader: a line when preparation starts; the observer logs the unpack's start and progress (percent, files, MB).
3. Page: the log beside the canvas, the elapsed time, line count and latest line, scrolling that follows, a copy button.
4. Checks: the Win32 build (the loader, `repiu_exe_analyzer`), the progress lines under Node, and in a headless browser
   lines from right after the start.
