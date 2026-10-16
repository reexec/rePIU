# 작업 지시: 브라우저 페이지의 소리 (issue #81)

설계: [20261017-i081](../design/20261017-i081-browser-sound.md)

브랜치: `task/i081-browser-sound`. 빌드 트리: Win32 `build/`, 웹 `~/repiu-web`(WSL) 또는 `build/web_wasm_win`(Windows emsdk).

## 순서

1. **확인:** SDL 오디오 하위 시스템 없이 `SDL_CreateAudioStream`·`SDL_PutAudioStreamData`·`SDL_GetAudioStreamData`가 되는지(Node).
2. **도우미:** `host_audio_stream.{h,cpp}`. 세 출력(YMZ280B, PIU10 MP3, CD 오디오)이 스트림을 이것으로 열고 닫음. 네이티브 동작은
   그대로.
3. **플랫폼:** `browser_host.h`에 `BrowserAudioSampleRate()`, `SendBrowserAudio(...)`. Win32·Linux는 0·무시, 웹은 EM_JS.
4. **믹서:** `browser_audio_mixer.{h,cpp}`, `OneThreadHostLoop::TakeTurn`에서 출력 `Pump()` 뒤에.
5. **페이지:** `audio_worklet.js`(링 버퍼, 상태 보고), main.js(Start에서 `AudioContext`·Worklet·`MessageChannel`, 상태 줄),
   worker.js(포트와 샘플률을 `Module`에), CMake의 페이지 파일 목록.
6. **검증:** 설계의 검증 1~4.
7. **문서:** 작업 로그, `ARCHITECTURE.md`, `src/platform/web/README.md`, `docs/analysis/web-port-frontier.md`, `docs/TODO.md`.

## 완료 기준

* 브라우저에서 pumpitea의 소리가 나온다(상태 줄의 진폭, 사용자 확인).
* Win32·Linux·Node 회귀 없음.

---

# Work order: sound in the browser page (issue #81)

Design: [20261017-i081](../design/20261017-i081-browser-sound.md)

Branch: `task/i081-browser-sound`. Build trees: Win32 `build/`, web `~/repiu-web` (WSL) or `build/web_wasm_win` (Windows
emsdk).

## Order

1. **Check:** whether `SDL_CreateAudioStream`, `SDL_PutAudioStreamData` and `SDL_GetAudioStreamData` work without the SDL
   audio subsystem (Node).
2. **Helper:** `host_audio_stream.{h,cpp}`; the three outputs (YMZ280B, PIU10 MP3, CD audio) open and close their streams
   through it. Native behaviour unchanged.
3. **Platform:** `BrowserAudioSampleRate()` and `SendBrowserAudio(...)` in `browser_host.h`; Win32 and Linux 0 and ignore,
   the web through EM_JS.
4. **Mixer:** `browser_audio_mixer.{h,cpp}`, in `OneThreadHostLoop::TakeTurn` after the outputs' `Pump()`.
5. **Page:** `audio_worklet.js` (ring buffer, state reports), main.js (the `AudioContext`, the worklet and the
   `MessageChannel` on Start, the state line), worker.js (the port and sample rate into `Module`), the page file list in
   CMake.
6. **Verification:** the design's checks 1 to 4.
7. **Documents:** the work log, `ARCHITECTURE.md`, `src/platform/web/README.md`, `docs/analysis/web-port-frontier.md`,
   `docs/TODO.md`.

## Done when

* pumpitea's sound plays in the browser (the peak on the state line, confirmed by the user).
* No Win32, Linux or Node regression.
