# 작업 로그: 브라우저 페이지의 소리 (issue #81)

설계: [20261017-i081](../design/20261017-i081-browser-sound.md) ·
작업 지시: [20261017-i081](../work-orders/20261017-i081-browser-sound.md)

## 2026-10-17 — 구현과 확인

브랜치 `task/i081-browser-sound`(main `7539353`, v0.0.226에서).

### 설계의 "확인 필요"에 대한 답

| 항목 | 결과 |
|---|---|
| 오디오 하위 시스템 없이 `SDL_CreateAudioStream` | 됨(드라이버 `none`, Node 시험 프로그램). S16 44.1 kHz → F32 48 kHz 변환, 음량, 장치 번호 0 모두 정상 |
| 게임이 느릴 때 MP3 | 브라우저 4분 동안 MP3 프레임 150개 디코드(약 4초 분량). 네이티브는 45초에 548개. attract 화면의 음악이 원래 짧고, 브라우저의 게임 속도가 실시간보다 느린 만큼 적음 |
| 메시지 빈도 | 한 메시지 최소 256프레임(48 kHz에서 약 5 ms). 5분 동안 문제없음 |

### 구현

| 부분 | 내용 |
|---|---|
| `host_audio_stream.{h,cpp}` | 세 출력이 스트림을 여닫는 곳. 네이티브는 기존과 같은 장치 스트림, 브라우저는 장치 없는 스트림(목적 형식 float 스테레오, 페이지 샘플률)을 믹서에 등록 |
| `browser_audio_mixer.{h,cpp}` | 호스트 차례마다 실시간만큼(앞서 100 ms, 250 ms 넘게 멈추면 기준 재설정) 꺼내 더하고 자른 뒤 보냄 |
| 출력 셋 | YMZ280B, PIU10 MP3, CD 오디오가 도우미를 씀(`audio_subsystem_open` 상태가 사라짐) |
| 플랫폼 | `BrowserAudioSampleRate()`, `SendBrowserAudio()`(웹: `HEAPF32.slice`를 넘김) |
| 페이지 | `audio_worklet.js`(1초 링 버퍼, 300 ms 넘으면 100 ms까지 버림, 1초마다 상태), main.js(Start에서 `AudioContext`·Worklet·`MessageChannel`, 상태 줄 `Sound: … ms buffered, peak …, … underruns`), worker.js(포트와 샘플률) |

### 브라우저 확인 (headless Chrome, `--autoplay-policy=no-user-gesture-required`, pumpitea 300초)

* 소리 상태: 버퍼 106~144 ms로 안정, **부족 0회**, 받은 소리가 실시간과 같은 속도로 늘어남(300초에 230초 분량, 처음 약 60초는 CHD 풀기).
* 81초 표본에서 진폭 0.30: 게임 소리가 Worklet까지 감. 나머지 표본은 0. 위의 MP3 양과 같은 설명.
* 화면: 약 85초부터 그림(소리 없던 #72 때 약 125초 — #79의 번역 덕분).
* 귀로 듣는 확인은 사용자에게(아래).

### 회귀

| 구성 | 결과 |
|---|---|
| Win32 `direct` Release | core probe 통과, pumpitea 45초 MP3 통계가 v0.0.222와 같음(받음 229,382 B, 디코드 548, 부족 1, 장치 버퍼 441프레임) |
| Win32 Debug, `emulated` Release | 빌드, core probe 통과 |
| Linux x64·i386 | 빌드, core probe 통과 |
| 웹 Node | core probe 통과, pumpitea 30초 정상 정지(12억 8,383만 단계, 페이지가 없어 출력은 장치를 못 열고 무음) |

### 남은 것

* 사용자 청취 확인: 코인을 넣고 곡을 고르면 효과음(YMZ280B)과 음악이 나오는지.
* 게임이 느린 동안의 음악 끊김은 처리량(#79 이후 rex86 작업)의 문제.

---

# Work log: sound in the browser page (issue #81)

Design: [20261017-i081](../design/20261017-i081-browser-sound.md) ·
Work order: [20261017-i081](../work-orders/20261017-i081-browser-sound.md)

## 2026-10-17 — implemented and checked

Branch `task/i081-browser-sound` (main `7539353`, from v0.0.226).

### Answers to the design's "to be confirmed"

| Item | Result |
|---|---|
| `SDL_CreateAudioStream` without the audio subsystem | works (driver `none`, a test program under Node): S16 44.1 kHz → F32 48 kHz conversion, gain and device id 0 all as expected |
| MP3 while the game is slow | 150 MP3 frames decoded in 4 minutes in the browser (about 4 s of music); natively 548 in 45 s. The attract screens carry little music to begin with, and the browser's game runs slower than real time by about that much |
| Message rate | at least 256 frames a message (about 5 ms at 48 kHz); no trouble over 5 minutes |

### Implementation

| Part | Content |
|---|---|
| `host_audio_stream.{h,cpp}` | where the three outputs open and close their streams: natively the same device stream as before, in a browser a stream with no device (target float stereo at the page's rate) registered with the mixer |
| `browser_audio_mixer.{h,cpp}` | at every host turn takes real time's worth (100 ms ahead; the base resets after more than 250 ms without a turn), sums, clips and sends |
| The three outputs | YMZ280B, PIU10 MP3 and CD audio use the helper (their `audio_subsystem_open` state is gone) |
| Platform | `BrowserAudioSampleRate()`, `SendBrowserAudio()` (the web hands over a `HEAPF32.slice`) |
| Page | `audio_worklet.js` (a 1 s ring, trimmed to 100 ms above 300 ms, state every second), main.js (the `AudioContext`, the worklet and the `MessageChannel` on Start, the state line `Sound: … ms buffered, peak …, … underruns`), worker.js (the port and the sample rate) |

### Browser check (headless Chrome, `--autoplay-policy=no-user-gesture-required`, pumpitea for 300 s)

* Sound state: 106 to 144 ms buffered and steady, **no underruns**, the sound received growing at real-time speed (230 s
  of it at 300 s; the first 60 s or so went to the CHD unpack).
* A peak of 0.30 at the 81 s sample: the game's sound reaches the worklet. The other samples are 0, explained by the MP3
  amount above.
* Picture: from about 85 s (about 125 s in #72, before sound — thanks to #79's translation).
* Listening is left to the user (below).

### Regressions

| Configuration | Result |
|---|---|
| Win32 `direct` Release | core probe passes; pumpitea's 45 s MP3 statistics match v0.0.222's (229,382 B received, 548 decoded, 1 starvation, 441-frame device buffer) |
| Win32 Debug, `emulated` Release | build, core probe passes |
| Linux x64 and i386 | build, core probe passes |
| Web under Node | core probe passes; pumpitea stops cleanly after 30 s (1,283.83 million steps; with no page the outputs open no device and stay silent) |

### Remaining

* The user's listening check: whether effects (YMZ280B) and music play after a coin and a song pick.
* Music breaking up while the game is slow is a throughput matter (rex86 work after #79).
