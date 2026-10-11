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

## 남은 것

* 수정은 하지 않았다. 후보: frame-sync 전이를 주입된 tick 하나당 최대 한 번으로 줄 세워, tick이 늦어도 게임이
  프레임을 잃지 않게 한다(두 OS 공통). Win32의 스왑 대기 중 tick 주입(#6에서 유보)은 ±15 ms 양자화를 줄인다.
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

## What remains

* No fix yet. Candidate: release frame-sync transitions at most one per injected tick, so late ticks never cost the
  game a frame (both OSes). Win32 tick injection during the swap wait (held back in #6) would shrink the ±15 ms.
* Numbers from the user's setups (Linux hardware, the Steam Deck, high-refresh screens, recording tools). WSL was
  not used for this measurement: its sound server's rate (Task 755) puts the MP3 and the ticks apart.
