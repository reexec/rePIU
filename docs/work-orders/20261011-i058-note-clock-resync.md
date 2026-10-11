# 작업 지시: 화살표 멈춤·되돌아감·점프의 원인 조사 (issue #58)

1. pumpipx3 PIU.EXE에서 곡 시계와 MP3 위치의 관계를 찾는다(타이머 ISR, MP3 공급 함수, 재동기 상수).
2. `Piu10SongClockCensus`: 게스트의 MP3 status 읽기마다 frame-sync 비트와 tick 카운터를 받아 게임과 같은
   256프레임 덮어쓰기를 재현하고, 덮어쓰기마다 화살표 점프(ms)와 그 창의 tick 공백·폐기 tick·토글 공백을 낸다.
   버퍼 스왑마다 프레임 사이 tick 수를 세어 화살표가 프레임마다 얼마나 움직였는지도 낸다.
3. 출력: `[repiu-song-clock] …` 줄(`REPIU_PIU10_SONG_CLOCK_LOG=1`이면 모든 덮어쓰기, 아니면 8 ms 이상만),
   최종 보고 `PIU10 song clock …`와 `PIU10 song frame ticks …`.
4. core probe `piu10_song_clock_census`: 깨끗한 곡, 정지 후 회수, tick 폐기, 곡 경계.
5. 자동 플레이(코인 5개 = 1크레딧, P1 가운데 발판)로 pumpipx3 실제 곡을 재생해 수치를 모은다.
6. 문서: `docs/analysis/piu-io-port-specification.md`에 곡 시계 절, README 진단 변수, 작업 로그.
7. 검증: Win32 Release 빌드와 core probe, Linux x64 빌드와 core probe.

완료 기준: 어떤 조건이 화살표를 움직이는지 수치로 가르고, 사용자가 실기에서 같은 로그를 낼 수 있다.

---

# Work order: what stalls, steps back and jumps the arrows (issue #58)

(1) Find in pumpipx3's PIU.EXE how the song clock relates to the MP3 position (the timer ISR, the MP3 feeder, the
resync constant). (2) `Piu10SongClockCensus`: fed the frame-sync bit and the tick counters on every guest read of the
MP3 status, it replays the game's 256-frame overwrite and gives each overwrite's arrow jump (ms) with the window's
tick gap, dropped ticks and toggle gap; on every buffer swap it counts the ticks between frames, which is how far the
arrows move from one frame to the next. (3) Output: `[repiu-song-clock] …` lines (every overwrite with
`REPIU_PIU10_SONG_CLOCK_LOG=1`, otherwise only those of 8 ms or more) and the final report's `PIU10 song clock …` and
`PIU10 song frame ticks …`. (4) Core probe `piu10_song_clock_census`: a clean song, a stall paid back, dropped ticks,
a song boundary. (5) An automated play (five coins a credit, the P1 center panel) through real pumpipx3 songs.
(6) Docs: a song clock section in `docs/analysis/piu-io-port-specification.md`, the README's diagnostic variables,
the work log. (7) Verify: Win32 Release build and core probe, Linux x64 build and core probe. Done when the numbers
tell which condition moves the arrows and the user can produce the same log on real hardware.
