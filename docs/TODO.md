# TODO

## 현재 상태

2026-10-08 기준 활성 항목은 셋입니다. **Linux x64 host 이식이 진행 중**이고, 웹 이식
Stage 3~5는 **보류 중**입니다 — 보류 이유는 우선순위이며 아래 항목에 적혀 있습니다.
**환경 변수 토글 정리**는 1번(#20)·2번(#22) 묶음을 마쳤고, 2차 조사에서 찾은 3번 묶음(#24)과
일회성 진단 정리(#25), 보류했던 guarded segment 스위치(#30)도 마쳤습니다. 남은 것은
판정 미정 실험 2개와 아직 판정하지 않은 진단 변수입니다.

Task 492~495의 JAMMA 입력 timing, IRQ0 replay, 2P 숫자패드 별칭과 history 정리는 사용자
120초 실행으로 최종 검증됐습니다.

현재 실행 우선순위와 확인된 병목은 [현재 실행 frontier](analysis/current-execution-frontier.md),
승인된 구현 단위는 [`work-orders/`](work-orders/), 완료 결과와 검증 근거는
[`work-logs/`](work-logs/)에서 관리합니다.

## 활성 항목

- **Linux x64 host 이식 — 진행 중(2026-09-01).** 상태와 다음 한 걸음은
  [Linux 이식 frontier](analysis/linux-port-frontier.md)의 **3.10 세션 인수인계**가
  정본입니다. 실행 모델은
  [Task 546 설계](design/20260831-546-linux-x64-aot-dbt-execution-model.md)이고,
  Tasks 549–559는 **`v0.0.176`으로 main에 머지됐습니다**.
  **한 줄로:** x64가 emitter의 바이트를 실제로 실행하고 guest 명령의 **66.17%**를 낼 수
  있지만, **완결 block은 2.66%**입니다 — block이 control flow로 끝나고 그것이 아직 하나도
  방출되지 않기 때문입니다. **명령의 2/3는 실행의 2/3가 아닙니다.**
  **다음 단위:** control flow — `CALL`/`RET`/`JMP`와 x64 dispatch slot·thunk·resolver
  (`not-a-copy-record` 12,856, Task 546 구현 순서 4단계). **이것 없이는 완결 block이 늘지
  않습니다.** 그 뒤가 `ESP`를 피연산자로 쓰는 일반 명령 재인코더(`stack-pointer` 6,401,
  x87 대부분 포함), 그리고 5단계 block 단위 대조입니다.
  **guest 실행 자체는 Task 544의 fail-closed 그대로입니다** — x64가 실행한 것은 probe가
  만든 프로그램이지 게임이 아닙니다.

- **웹(WebAssembly) 이식 — 재개(2026-10-14, issue #61).** 상태는 [웹 이식 frontier](analysis/web-port-frontier.md)
  0절이 정본입니다. 네이티브 Windows에서 rex86 위의 `emulated` 실행 모델로 pumpitea가 타이틀과 데모 플레이까지
  나옵니다([설계](design/20261014-i061-emulated-execution-model.md)). 실행 규칙은 `dynamic` 백엔드의 HLE 경계 분류를
  그대로 씁니다. Windows 인터프리터는 약 9.3 MIPS이고, 실시간에는 약 75 MIPS가 필요한 것으로 추정합니다.
  **브라우저 호스트 — issue #64.** 단계 1(단일 스레드 호스트 루프)을 마쳤습니다: `emulated`는 게스트·Glide·tick·
  오디오·입력을 한 스레드에서 번갈아 돌립니다([설계](design/20261015-i064-browser-host-single-thread-loop.md)).
  타이틀·소리는 사용자 확인이 남아 있습니다.
  **단계 2 — issue #67:** 엔진과 rex86이 wasm32로 빌드되어 Node에서 pumpitea가 돕니다(약 6.1 MIPS, 경계 trace가
  네이티브와 첫 벽시계 의존까지 같음, [설계](design/20261015-i067-emscripten-headless.md)).
  **다음:** 단계 3(GLES 3 / WebGL2 렌더러), 단계 4(페이지), 그리고 대기 루프를 뺀 장면별 처리량 측정과 rex86 번역 범위
  확대(x87, REP 문자열).

- **환경 변수 토글 정리 — 1~3번 묶음, 일회성 진단(#25), guarded segment(#30) 완료(2026-10-09).** 조사 결과와 전체
  목록은 [환경 변수 토글 목록](analysis/environment-toggle-inventory.md)이 정본입니다.
  기능 토글 약 45개 중 **1번 묶음**(오래 승격된 기능의 끄기 스위치 15개)은 issue #20에서,
  **2번 묶음**(버려진 옵트인 실험 11개 묶음의 기능·스위치)은 issue #22에서 삭제했습니다.
  2026-10-09 2차 조사에서 진단 이름 뒤에 숨은 토글을 더 찾았습니다.
  * **3번 묶음 — issue #24.** 끄기 스위치 3개(`REPIU_GLIDE_DRAW_ENTRY_POINTS`,
    `REPIU_TIMER_TICK_BACKLOG`, `REPIU_JAMMA_SNAPSHOT`)와 실험 2개
    (`REPIU_DOS4GW_MEMORY_PATH_PROBE`, `REPIU_DPMI_1E7F_PROBE_SUCCESS`와 같은 블록의
    `_TRACE`). v0.0.209에서 삭제했습니다(probe와 pumpit2a 실행으로 검증). 1E7F는 Task 606에서 원인이 스택 손상으로 확정돼 실행 없이 지웠습니다.
  * **일회성 진단 — issue #25.** 후보 13개 중 9개를 지우고, 살아 있는 기구의 계측이거나
    가이드·스크립트가 쓰는 4개(`CALL_TRACE`, `RETIRED_TRAP_PROFILE`, `SETTER_CENSUS`/`_PHASE`)는
    근거와 함께 남겼습니다.
  * **guarded segment 끄기 스위치 — issue #30.** 보류했던 `REPIU_AOT_GUARDED_SEGMENT_LOAD/
    POP/READ`를 v0.0.207~v0.0.210 동안 새 슬롯 회귀가 없어 지웠습니다.
  남은 것(아직 issue 없음):
  * **판정 미정 실험 2개.** `REPIU_GLIDE_ASYNC_PRESENT`(09-28 측정 중)와
    `REPIU_GLIDE_LFB_STAGING_REUSE`(09-22). 채택(기본 켜기)이나 기능째 삭제 중 결론이 필요합니다.
  * **판정하지 않은 진단 변수.** 2026-08 중순 이후 언급된 68개(`LINUX_X64_*` 추적 약 20개 포함)와
    그 이전의 약 20개. 범위를 좁혀 필요할 때 진행합니다.
  * **최근 승격된 끄기 스위치 11개**(목록의 "유지" 절)는 안정되면 1번 묶음처럼 정리할 후보입니다.

## 이번 정리에서 닫힌 이전 항목

- **Task 492 최초 live 검증:** 입력 edge와 due queue에는 유실이 없었지만 IF gate 때문에
  replay가 소비되지 않는 사실을 확인했고 Task 493에서 수정했습니다.
- **Task 494 2P 숫자패드 재검증:** 사용자 로그에서 다섯 위치가 모두 균형 잡힌
  press/release transition을 기록해 keycode 별칭 수정을 확인했습니다.
- **Task 495 JAMMA history 재검증:** 120초 사용자 실행에서 1,006 edge를 모두 안전하게
  정리했고 `history-peak=4`, 모든 timeline loss counter 0을 확인했습니다.
- **Task 365 Glide setter/batching 후속:** Task 439, 443, 444에서 구현·검증·기본값 승격을
  완료했습니다.
- **Port I/O `0x02A0` 계열 의미:** Task 290에서 YMZ280B register/data byte lane으로
  확인하고 sound HLE에 연결했습니다.
- **Task 445에서 발견된 stall watchdog 오판:** Task 490에서 wall timeout과 stall timeout을
  분리하고 Glide direct dispatch를 진행 신호에 포함해 완료했습니다.

## 항목 추가 기준

특정 작업의 일시적인 다음 관측점은 해당 work log나 analysis에 기록합니다. 여러 작업에
걸쳐 의도적으로 보류하며 별도의 재개 조건이 있는 미완료 항목만 이 문서에 추가합니다.

---

# TODO

## Current Status

As of 2026-10-08 there are three items: the Linux x64 host port is in progress, Stages 3
through 5 of the web port are **on hold** -- the Linux performance axis comes first -- and the
**environment toggle cleanup** has finished groups 1 (#20) and 2 (#22), has finished group 3
(#24) and the one-off diagnostics (#25) from a second survey, and the deferred guarded
segment switches (#30); two undecided experiments and unreviewed diagnostics remain. A 120-second user run
completed final validation
of Tasks 492 through 495: JAMMA input timing, IRQ0 replay, 2P numpad aliases, and history pruning.

Current execution priorities and confirmed bottlenecks live in the
[current execution frontier](analysis/current-execution-frontier.md), approved implementation
units in [`work-orders/`](work-orders/), and completed results and verification evidence in
[`work-logs/`](work-logs/).

## Active Item

- **Web (WebAssembly) port -- resumed (2026-10-14, issue #61).** Section 0 of the
  [web port frontier](analysis/web-port-frontier.md) is the state of record. On native Windows, the `emulated` execution
  model on rex86 takes pumpitea to its title and demo play ([design](design/20261014-i061-emulated-execution-model.md)),
  running by the `dynamic` backend's HLE boundary classification. The Windows interpreter runs about 9.3 MIPS; real time
  is estimated to need about 75 MIPS. **The browser host -- issue #64.** Phase 1 (the single-threaded host loop) is
  done: `emulated` runs the guest, Glide, ticks, audio and input on one thread, taking turns
  ([design](design/20261015-i064-browser-host-single-thread-loop.md)); the title and the sound are left for the user to
  confirm. **Phase 2 -- issue #67:** the engine and rex86 build for wasm32 and pumpitea runs under Node (about 6.1 MIPS,
  the boundary trace matching native up to the first wall-clock dependence,
  [design](design/20261015-i067-emscripten-headless.md)). **Next:** phase 3 (the GLES 3 / WebGL2 renderer), phase 4
  (the page), per-scene throughput measured with the wait loops taken out, and wider rex86 translation (the x87, REP
  strings).

- **Environment toggle cleanup -- groups 1 to 3, the one-off diagnostics (#25) and the guarded segment switches (#30) done (2026-10-09).** The
  survey and full list live in the [environment toggle inventory](analysis/environment-toggle-inventory.md).
  Of about 45 feature toggles, **group 1** (15 kill switches of long-promoted features) was
  deleted in issue #20 and **group 2** (feature and switch of 11 abandoned opt-in experiments)
  in issue #22. A second survey on 2026-10-09 found more toggles behind diagnostic names.
  * **Group 3 -- issue #24.** Three kill switches (`REPIU_GLIDE_DRAW_ENTRY_POINTS`,
    `REPIU_TIMER_TICK_BACKLOG`, `REPIU_JAMMA_SNAPSHOT`) and two experiments
    (`REPIU_DOS4GW_MEMORY_PATH_PROBE`, `REPIU_DPMI_1E7F_PROBE_SUCCESS` with the block's
    `_TRACE`), deleted in v0.0.209 (verified by probes and pumpit2a runs). 1E7F went without a run because Task 606 traced the call to stack corruption.
  * **One-off diagnostics -- issue #25.** Nine of the thirteen candidates deleted; four kept
    with their reasons because they measure a live mechanism or a guide or script uses them
    (`CALL_TRACE`, `RETIRED_TRAP_PROFILE`, `SETTER_CENSUS`/`_PHASE`).
  * **Guarded segment kill switches -- issue #30.** The deferred
    `REPIU_AOT_GUARDED_SEGMENT_LOAD/POP/READ` went after v0.0.207 to v0.0.210 showed no
    regression against the new slots.
  Remaining (no issue yet):
  * **Two undecided experiments**, `REPIU_GLIDE_ASYNC_PRESENT` (being measured on 09-28) and
    `REPIU_GLIDE_LFB_STAGING_REUSE` (09-22), need a verdict: adopt as default-on or delete.
  * **Unreviewed diagnostics**: 68 mentioned from mid-2026-08 on (about 20 of them
    `LINUX_X64_*` traces) and about 20 earlier ones; to be narrowed and done when needed.
  * **Eleven recently promoted kill switches** (the inventory's "keep" list) become group-1
    style candidates once they settle.

## Previous Entries Closed by This Cleanup

- **Initial Task 492 live validation:** it confirmed no edge or due-queue loss but found that the IF
  gate prevented replay consumption; Task 493 corrected it.
- **Task 494 2P numpad revalidation:** the user log recorded balanced press/release transitions for
  all five positions, confirming the keycode-alias correction.
- **Task 495 JAMMA history revalidation:** a 120-second user run safely pruned all 1,006 edges,
  peaked at four retained entries, and reported zero for every timeline loss counter.
- **Task 365 Glide setter/batching follow-ups:** Tasks 439, 443, and 444 completed implementation,
  verification, and default promotion.
- **Meaning of the `0x02A0` Port-I/O family:** Task 290 identified YMZ280B register/data byte lanes
  and connected them to sound HLE.
- **The stall-watchdog false positive found in Task 445:** Task 490 separated wall and stall
  timeouts and added Glide direct dispatch to the progress definition.

## Entry Criteria

Keep a task's temporary next observation point in its work log or cumulative analysis. Add an item
here only when unfinished work is intentionally deferred across tasks and has an explicit condition
for resuming it.
