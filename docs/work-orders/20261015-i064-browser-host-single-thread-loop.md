# 작업 지시: 브라우저 호스트 단계 1 — `emulated` 모델의 단일 스레드 호스트 루프 (issue #64)

설계: [20261015-i064](../design/20261015-i064-browser-host-single-thread-loop.md)

브랜치: `web-browser-host`. 빌드 트리: `build/` (`direct`), `build/win32_x86_emulated` (`emulated`).

## 순서

1. **실행 모델 질문:** `include/repiu/runtime/execution_model.h`에 `RunsHostOnOneThread()`. `direct`·`cache`·`none`은
   false, `emulated`는 true(`src/runtime/execution_model_*.cpp`).
2. **호스트 차례 추출:** `PollThreadUntilExit`의 반복 몸통을 `src/engine/execution/host_turn.{h,cpp}`(가칭)로.
   반복을 넘어 이어지는 상태는 호스트 차례 객체가 가진다. 폴링 루프는 "호스트 차례 → 기다림"만 남긴다.
   **이 단계만으로 `direct` 빌드·probe·pumpitea 회귀 확인 후 커밋.**
3. **양보:** `RequestHostTurn(context)`. `direct`에서는 지금의 짧은 잠, 단일 스레드에서는 슬라이스 끝 플래그.
   INT 21h getch의 `sleep_for`를 이것으로 바꾼다.
4. **Glide 교착 방지:** 단일 스레드에서 랑데부·back pressure의 조건 변수 대기에 닿으면 진단을 남기고 실패.
   즉시 실행 계수를 최종 보고에 한 줄.
5. **오디오 펌프:** YMZ280B, PIU10 MP3, CD 오디오 워커의 반복 몸통을 `Pump()`로. 스레드 모델은 스레드가
   `Pump()`를 반복, 단일 스레드는 호스트 차례가 부른다. CD 오디오의 구조는 먼저 확인.
6. **입력 스크립트:** `SdlInputScriptPlayer::Pump(now)`. 어려우면 단일 스레드에서 거절하고 이유 출력.
7. **단일 스레드 루프:** `emulated`에서 게스트 스레드를 만들지 않고, 호출한 스레드에서 `EnterGuestEmulated`의
   슬라이스와 호스트 차례를 번갈아 돈다(설계 결정 3의 넘김 조건). SEH·vectored handler가 필요한지 확인한다.
8. **정지·종료:** 창 닫기, 종료 조합, 시간 제한, 정체 감시를 호스트 차례의 결과로. 종료 보고·코드는 지금과 같게.
9. **보고:** 최종 보고에 `host loop: one thread`(가칭) 한 줄과 호스트 차례 수·평균 간격.
10. **검증:** 설계의 검증 1~5. 360초 실행으로 처리량 비교. 긴 실행은 필요한 만큼만.
11. **문서:** 작업 로그, `ARCHITECTURE.md`의 실행 모델 절(호스트 루프 두 모양), `docs/analysis/web-port-frontier.md`
    0절, `docs/TODO.md`.

## #61 후속 (같은 브랜치, 커밋 제목은 `(#61)`)

* `AGENTS.md`, `docs/CODING_STYLE.md`, `ARCHITECTURE.md`가 실행 모델을 둘(`direct`, `cache`)로 적고 있다.
  `emulated`(`<이름>_emulated.cpp`)를 더해 셋으로 고친다.

## 완료 기준

* Windows `emulated` Release가 단일 스레드 루프로 pumpitea 타이틀·데모 플레이까지 나오고, 소리·입력·종료가 동작한다.
* 게스트 실행 중 프로세스가 만든 스레드가 메인 하나뿐이다.
* 단계 1 전후 `emulated`의 HLE 경계 trace가 첫 벽시계 의존까지 같다.
* `direct` 빌드는 회귀가 없다(core probe 44/44, pumpitea `dynamic`). WSL의 Linux i386·x64 빌드와 core probe도 통과한다.

---

# Work order: browser host phase 1 — a single-threaded host loop for the `emulated` model (issue #64)

Design: [20261015-i064](../design/20261015-i064-browser-host-single-thread-loop.md)

Branch: `web-browser-host`. Build trees: `build/` (`direct`), `build/win32_x86_emulated` (`emulated`).

## Order

1. **Execution model question:** `RunsHostOnOneThread()` in `include/repiu/runtime/execution_model.h`; false on
   `direct`, `cache` and `none`, true on `emulated` (`src/runtime/execution_model_*.cpp`).
2. **Extract the host turn:** the body of `PollThreadUntilExit`'s iteration moves to `src/engine/execution/host_turn.{h,cpp}`
   (working name); state that carries across iterations belongs to the host-turn object; the poll loop keeps only
   "host turn → wait". **Commit after this step alone passes the `direct` build, probes and pumpitea.**
3. **Yield:** `RequestHostTurn(context)`: today's brief sleep on `direct`, a slice-ending flag on one thread. It
   replaces INT 21h getch's `sleep_for`.
4. **Glide deadlock guard:** on one thread, reaching the rendezvous or back-pressure condition-variable wait leaves a
   diagnostic and fails; the in-place execution count goes on one final-report line.
5. **Audio pumps:** the loop bodies of the YMZ280B, PIU10 MP3 and CD audio workers become `Pump()`. On threaded models
   the thread repeats `Pump()`; on one thread the host turn calls it. Check the CD audio structure first.
6. **Input script:** `SdlInputScriptPlayer::Pump(now)`; if that is hard, refuse it on one thread with a printed reason.
7. **The single-threaded loop:** on `emulated`, create no guest thread; the calling thread alternates
   `EnterGuestEmulated`'s slices with host turns (the hand-over conditions of design decision 3). Check whether SEH and
   the vectored handler are needed.
8. **Stop and exit:** closing the window, the exit chord, the timeout and the stall watch as host-turn results; the exit
   report and code as today.
9. **Report:** a `host loop: one thread` (working name) line in the final report, with the host-turn count and mean gap.
10. **Verification:** the design's verification 1 to 5; throughput compared over a 360 s run; long runs only as needed.
11. **Documents:** the work log, `ARCHITECTURE.md`'s execution model section (the host loop's two shapes),
    section 0 of `docs/analysis/web-port-frontier.md`, and `docs/TODO.md`.

## #61 follow-up (same branch, commit titles ending `(#61)`)

* `AGENTS.md`, `docs/CODING_STYLE.md` and `ARCHITECTURE.md` describe two execution models (`direct`, `cache`); add
  `emulated` (`<name>_emulated.cpp`) to make three.

## Done when

* The Windows `emulated` Release reaches pumpitea's title and demo play on the single-threaded loop, with sound, input
  and exit working.
* While the guest runs, the process has created only the main thread.
* `emulated`'s HLE boundary trace before and after phase 1 agrees up to the first wall-clock dependence.
* The `direct` build does not regress (core probe 44/44, pumpitea `dynamic`); the Linux i386 and x64 builds and core
  probes under WSL pass too.
