# Issue #58 작업 로그 — 화살표 멈춤·되돌아감·점프의 원인

작업 지시: [20261011-i058](../work-orders/20261011-i058-note-clock-resync.md)

## 요약

pumpipx3의 화살표는 타이머 tick으로 올라가는 곡 시계를 따르고, 게임은 MP3 256프레임(약 6.7초)마다 그 시계를
`프레임 수 × 6.2688`로 덮어쓴다. 게임이 세는 MP3 프레임 수는 frame-sync 비트의 변화를 ISR에서 본 횟수다.
tick이 26 ms 넘게 끊기면 토글 두 개가 상쇄되어 프레임 수가 영구히 2프레임(52 ms) 뒤처지고, 다음 덮어쓰기에서
화살표가 뒤로 가며 그 곡 내내 늦다. 이것이 큰 되돌아감의 기전이다. 자동 플레이에서 −95 ~ −195 ms 되돌아감을
재현했는데, 원인은 동시에 돌린 창 캡처가 만든 present 정체였다. 캡처 없이 돌리면 스테이지는 ±5~15 ms(Win32의
vsync 대기 중 tick 정지에 따른 관측 양자화)였고, 토글 손실은 곡 전환 로딩에서만 나왔다. 덮어쓰기마다 점프를
재는 진단을 넣어 사용자 환경에서 같은 수치를 받을 수 있게 했다.

## 과정

1. 포트 census에서 0x2DA 접근이 공용 `inpw`(`0x040FC0A1`)를 거치는 것을 보고, LE 이미지를 풀어 24개 호출부 중
   `and eax,4`로 frame-sync를 다루는 세 곳(`0x0402D846`, `0x0402D904`, `0x0402D96F`)을 찾았다. 공급 함수
   `0x0402D86B`가 256프레임마다 setter `0x040121CD`를 부르고, 그 대상 `[0x92aaf8]`를 타이머 ISR `0x0401213D`가
   tick마다 올린다. 상수는 fixup을 풀어 object 4 + `0x180D`의 float 6.2688.
2. `Piu10SongClockCensus`: status 읽기마다 frame-sync와 주입·폐기 tick 수를 받아 게임과 같은 256프레임
   덮어쓰기를 재현한다. 점프 = 256프레임의 오디오 시간 − 그 창에 주입된 tick 시간(게임 상수 대신 호스트의
   PIT × 1152/44100, 차이는 덮어쓰기당 −1.9 ms). 버퍼 스왑마다 프레임 사이 tick 수도 센다.
3. 자동 플레이 경로: 이 롬셋은 코인 5개가 1크레딧이고(타이틀 `CREDIT(S) 0 (3/5)`), 082.AUD는 데모가 아니라
   타이틀 BGM이다. 코인 15개 + P1 가운데 발판 3초 간격으로 스테이지(821.AUD)를 두 번 재생했다.
4. 캡처를 켠 실행(play3)에서 −111/−195/−95 ms 되돌아감과 토글 손실 16건. 손실 직전 프레임은 present 29–65 ms
   (게스트 1–4 ms) 또는 게스트 190–480 ms(로딩).
5. 캡처 없는 실행(play4)에서 스테이지 덮어쓰기 −15.4 ~ +5.4 ms, 긴 present 24 → 7, 손실 4건은 모두 스테이지
   밖 곡 전환.

## 검증

| 검증 | 결과 |
|---|---|
| Win32 x86 Release 빌드(`repiu`, `repiu_core_probe`) | 성공(기존 C4819 경고) |
| Win32 core probe | `piu10_song_clock_census_probe=true`, `core_probe_all=true` |
| Linux x64 Release 빌드 + core probe(WSL) | 성공, `piu10_song_clock_census_probe=true`, `core_probe_all=true` |
| pumpipx3 자동 플레이 300초 ×2(캡처 有/無) | 위 표와 `docs/analysis/piu-io-port-specification.md` 22절 |

로그는 세션 임시 디렉터리에 있다(`px3_play3.err.log`, `px3_play4.err.log`).

## 2차 분석: 타이머 tick 전달의 한계

사용자 요청으로 tick이 어디서 늦는지 계측했다(`REPIU_TIMER_LATENCY_TRACE`, `docs/analysis/interrupts-and-port-io.md`의
issue #58 절).

* Win32(실제 vsync): tick 지연의 거의 전부가 arm 지연(p50 6.05 ms, p90 13.2 ms)이다. PIT 스케줄이 vsync present를
  실행하는 호스트 폴 스레드에 있어 present가 막힌 동안 tick이 등록되지 않고, 게스트 스레드도 그 present를
  grBufferSwap·grTexDownloadTable·grTexSource 게이트에서 프레임당 약 14 ms 기다린다. tick은 프레임당 한 번 4개씩
  몰려 들어가(4개 묶음 9,915번) ISR이 사실상 60 Hz로 장치를 읽는다.
* Linux x64(WSL, 페이서): arm 지연 p50 0.23 ms, tick 96%가 1 ms 안. WSL에서는 MP3 재생이 진행되지 않아 토글은
  비교하지 못했다.
* 토글 손실(캡처 없음, Win32): 곡 시작·곡 전환에서 게스트가 88–140 ms 동안 tick을 못 받은 구간(주입 거부는
  cli·IF·in-service가 몇 번뿐 — 안전점에 거의 닿지 않음)과, 스테이지 시작의 73 ms grBufferSwap.
* INT 21h 기록은 0건이었다. 0.5 ms를 넘는 서비스가 없었거나 파일 읽기가 이 핸들러를 거치지 않는 경로로 간다(미확인).

## 수정 A: frame-sync를 tick의 due 시각으로 답한다

사용자 결정으로 A(MP3 상태 비트를 키 입력처럼 tick 시각 기준으로 답함)를 먼저 했다. B(tick 스케줄 전용 스레드)와
C(모든 게이트 대기 중 주입)는 공존 가능하고 함께 해야 효과가 나므로 별도 과제로 둔다.

* `Piu10FrameSyncHistory`: 워커가 뒤집은 시각과 값을 256칸 링에 기록, 게스트가 과거 시각의 값을 조회.
* `Piu10FrameSyncAnswerClock`: 답할 시각 = 게스트 시각(핸들러 안은 그 tick의 due, 밖은 마지막 전달 tick의 due),
  지금을 넘지 않고 뒤로 가지 않음.
* `JammaInputTimeline::TryReplayTimestamp`: 키 입력과 같은 frame 규칙으로 due 시각, 밖이면 마지막 전달 시각.
* 포트 I/O가 PIU10 읽기 직전 ESP를 `ThreadContext::piu10_read_guest_esp`에 둔다.
* 스위치 `REPIU_PIU10_FRAME_SYNC_REPLAY`(기본 켜짐). 트레이스에 `read` 이벤트 추가.

| 실행(Win32, 캡처 5초 간격, 300초) | 스테이지 덮어쓰기 점프 | 비고 |
|---|---|---|
| 수정 전(play3) | −194.6 ~ +5.4 ms, 8 ms 이상 8회 | |
| 첫 판: 핸들러 밖을 "지금"으로(play6, play7) | 대부분 ±3 ms, 그러나 −448.7, −190.4, +51.1 | 상태 읽기의 70%가 핸들러 밖(CAT702) |
| 최종(play8) | 16회 모두 +1.1 / −2.9 ms | 타이틀 곡 +26.1 ms 1회 = 290 ms 정지의 tick 폐기 |

캡처 없는 평상 조건(play9, 300초, 평균 58.5 fps): 덮어쓰기 21회 중 20회가 −2.9 ~ +5.3 ms. 스테이지 중간에 게스트가
440 ms 멈춘 창 하나(그린 프레임 351/401, tick 50개 폐기)가 +47.0 ms였다. 정지 원인은 이 실행에 트레이스가 없어
미확인이며, frame-sync가 아니라 정지와 backlog 상한의 문제다.

정지 재현 시도: 트레이스를 켠 실행 4회(300초 1회, 160초 3회)에서 스테이지 정지는 다시 나오지 않았고 덮어쓰기는
모두 ±5.4 ms 이내였다. play9의 정지 구간에서는 호스트 폴 스레드의 1초 live 줄 간격도 1,123 ms로 벌어졌고 몇 초간
fps가 38–62로 출렁였다. 게스트만이 아니라 호스트 스레드까지 막힌 정지로 보이며(추정), 그 동안 due tick이 한 번에
64개 상한을 넘어 버려졌다.

검증: Win32 Release `repiu`·`repiu_core_probe`·`repiu_aot_probe` 빌드, core probe `core_probe_all=true`
(`piu10_frame_sync_history_probe=true`), `repiu_aot_probe --jamma-input-timeline` true, Linux x64 Release 빌드와 core probe(WSL) `core_probe_all=true`.

## 남은 것

* B와 C: tick 스케줄을 present와 분리하고, 모든 게이트 대기 중에 tick을 주입한다. #6의 프레임 15% 비용 원인부터.
* 스테이지 중 게스트 정지(440 ms, fps 38)의 원인과, 267 ms 넘는 정지의 tick 폐기(backlog 64).
* 사용자 환경(Linux 실기, 스팀덱, 고주사율 화면, 녹화 도구)의 수치. WSL은 사운드 서버 속도 문제(Task 755)로
  MP3와 tick이 어긋나 이 측정에 쓰지 않았다.

---

# English

# Issue #58 work log — what stalls, steps back and jumps the arrows

Work order: [20261011-i058](../work-orders/20261011-i058-note-clock-resync.md)

## Summary

pumpipx3's arrows follow a song clock that timer ticks advance, and every 256 MP3 frames (about 6.7 s) the game
overwrites it with `frames × 6.2688`. The frame count is how many frame-sync changes the ISR has seen. When ticks
stop for more than 26 ms two toggles cancel, the count falls two frames (52 ms) behind for good, and the next
overwrite moves the arrows back and leaves them late for the rest of the song: the mechanism of the large backward
steps. An automated play reproduced steps of −95 to −195 ms, caused by present stalls from a window capture running
alongside; without the capture the stages stayed within ±5–15 ms (the observation quantization of Win32, whose vsync
wait carries no ticks) and toggles were lost only in song-change loads. A diagnostic measuring every overwrite's jump
lets the user's setups give the same numbers.

## Steps

1. The port census showed 0x2DA going through the shared `inpw` (`0x040FC0A1`); unpacking the LE image, three of its
   24 callers handle frame-sync with `and eax,4` (`0x0402D846`, `0x0402D904`, `0x0402D96F`). The feeder `0x0402D86B`
   calls the setter `0x040121CD` every 256 frames, and the timer ISR `0x0401213D` advances its target `[0x92aaf8]`
   on every tick. The constant, from the fixup, is the float 6.2688 at object 4 + `0x180D`.
2. `Piu10SongClockCensus`: fed the frame-sync bit and the injected and dropped tick counts on every status read, it
   replays the game's 256-frame overwrite. Jump = audio time of 256 frames − tick time injected over the window (the
   host's PIT × 1152/44100 in place of the game's constant, −1.9 ms per overwrite apart). It also counts the ticks
   between buffer swaps.
3. The automated play: this set takes five coins a credit (the title reads `CREDIT(S) 0 (3/5)`), and 082.AUD is the
   title music, not a demo. Fifteen coins and the P1 center panel every 3 s played the stage (821.AUD) twice.
4. With the capture on (play3): steps back of −111/−195/−95 ms and 16 lost-toggle events. The frames before each loss
   had a 29–65 ms present (guest 1–4 ms) or a 190–480 ms guest (a load).
5. Without it (play4): stage overwrites −15.4 to +5.4 ms, long presents 24 → 7, four losses all in song changes
   outside the stages.

## Verification

| Check | Result |
|---|---|
| Win32 x86 Release build (`repiu`, `repiu_core_probe`) | succeeded (existing C4819 warnings) |
| Win32 core probe | `piu10_song_clock_census_probe=true`, `core_probe_all=true` |
| Linux x64 Release build + core probe (WSL) | succeeded, `piu10_song_clock_census_probe=true`, `core_probe_all=true` |
| pumpipx3 automated play, 300 s × 2 (capture on/off) | the table above and section 22 of `docs/analysis/piu-io-port-specification.md` |

The logs are in the session's temporary directory (`px3_play3.err.log`, `px3_play4.err.log`).

## Second analysis: the limits of timer tick delivery

At the user's request, where ticks are held was measured (`REPIU_TIMER_LATENCY_TRACE`, the issue #58 section of
`docs/analysis/interrupts-and-port-io.md`). On Win32 with real vsync almost all tick latency is arming lateness
(p50 6.05 ms, p90 13.2 ms): the PIT schedule lives on the host poll thread that runs the vsync present, so no tick is
registered while the present blocks, and the guest thread waits for that present in the grBufferSwap,
grTexDownloadTable and grTexSource gates for about 14 ms a frame. Ticks arrive four at once, once a frame (9,915
four-tick bursts), so the ISR reads its devices effectively at 60 Hz. On Linux x64 (WSL, pacer) arming lateness is
p50 0.23 ms and 96% of ticks land within 1 ms; MP3 playback did not advance on WSL, so toggles were not compared.
Toggle losses without the capture (Win32): song starts and changes where the guest got no tick for 88–140 ms (a few
cli, IF and in-service refusals only, so safe points were hardly reached), and a 73 ms grBufferSwap at a stage start.
The INT 21h record stayed empty: either no service passed 0.5 ms or file reads take a path around this handler
(unconfirmed).

## Fix A: frame-sync answered at the tick's due time

The user chose A first (answer the MP3 status bit at tick time, as the keys are). B (a thread of its own for the tick
schedule) and C (injection during every gate wait) can coexist and work best together; they are a task of their own.

* `Piu10FrameSyncHistory`: the worker records each flip's time and value in a 256-entry ring; the guest looks up the
  value at a past time. `Piu10FrameSyncAnswerClock`: the time answered is the guest's (the tick's due time inside a
  handler, the latest delivered tick's outside), never past the present and never backwards.
  `JammaInputTimeline::TryReplayTimestamp` gives that time by the keys' frame rules. The port I/O path leaves the
  ESP of a PIU10 read in `ThreadContext::piu10_read_guest_esp`. Switch `REPIU_PIU10_FRAME_SYNC_REPLAY` (on); a
  `read` event in the trace.
* Win32, capture every 5 s, 300 s: before the fix (play3) stage overwrites −194.6 to +5.4 ms, eight over 8 ms; a first
  version answering reads outside handlers at the present (play6, play7) mostly ±3 ms but −448.7, −190.4 and +51.1,
  with 70% of status reads outside handlers (CAT702); the final one (play8) all 16 at +1.1 or −2.9 ms, plus one
  +26.1 ms in the title song from ticks dropped in a 290 ms stall.

Normal conditions without the capture (play9, 300 s, 58.5 fps on average): 20 of 21 overwrites within −2.9 to +5.3
ms. One window, where the guest stood still for 440 ms mid-stage (351 of 401 frames drawn, 50 ticks dropped), gave
+47.0 ms. The stall's cause is not known (this run had no trace); it is a stall and the backlog bound, not frame-sync.

Attempts to reproduce the stall: four traced runs (one of 300 s, three of 160 s) showed no stage stall and every
overwrite within ±5.4 ms. In play9's stalled window the host poll thread's one-second live line also came 1,123 ms
apart and fps swung between 38 and 62 for a few seconds: a stall that held the host threads as well as the guest
(inferred), during which the due ticks passed the 64-tick bound at once and were dropped.

Verification: Win32 Release `repiu`, `repiu_core_probe` and `repiu_aot_probe` builds, core probe `core_probe_all=true`
(`piu10_frame_sync_history_probe=true`), `repiu_aot_probe --jamma-input-timeline` true, Linux x64 Release build and core probe (WSL) `core_probe_all=true`.

## What remains

* B and C: the tick schedule apart from the present, and injection during every gate wait, starting from the cause of
  #6's 15% frame cost.
* The cause of a mid-stage guest stall (440 ms, 38 fps), and ticks dropped in stalls over 267 ms (the 64-tick backlog).
* Numbers from the user's setups (Linux hardware, the Steam Deck, high-refresh screens, recording tools). WSL was
  not used for this measurement: its sound server's rate (Task 755) puts the MP3 and the ticks apart.
