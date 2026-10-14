# 작업 지시: rex86 위의 `emulated` 실행 모델, 네이티브 먼저 (issue #61)

설계: [20261014-i061](../design/20261014-i061-emulated-execution-model.md)

## rex86 (별도 issue, `../rex86`)

1. `Features::user_mode`(가칭, 기본 꺼짐): CPL 3·IOPL 0 의미. CLI, STI, HLT, IN, OUT, INS, OUTS는 실행 전
   특권 폴트, INT n(n≠3)은 실행 전 #GP. EIP는 그 명령을 가리킨다.
2. 단위 테스트(명령마다 실행 전 폴트, 상태 불변)와 기존 테스트·probe 전체 통과. 다섯 호스트 CI.
3. 머지와 tag는 사용자 지시로. 그 전까지 rePIU는 `FETCHCONTENT_SOURCE_DIR_REX86`으로 로컬 rex86을 쓴다.

## rePIU

1. **빌드:** `REPIU_EXECUTION_MODEL=emulated`, rex86 FetchContent(tag 고정 + 로컬 소스 덮어쓰기), rex86이 있는
   구성에서 `repiu_zydis` 대신 `rex86_zydis`. 빌드 트리 `build/win32_x86_emulated`.
2. **모델 응답:** `execution_model_emulated.cpp`. 게스트 바이트 직접 실행 아님, long mode 캐시 아님, 복귀
   thunk 0, 스왑 대기 중 tick 주입 여부는 `direct`와 같게 둘지 확인 후 결정.
3. **Environment:** 호스트 셀렉터만 평탄 디스크립터로 받기(FS base = TEB), 포트 I/O와 인터럽트 대상은 모두 거절,
   RDTSC·CPUID는 호스트 값.
4. **상태 변환:** `CpuState` ↔ `GuestCpuContext`(GPR, EIP, EFLAGS, 셀렉터, FNSAVE 형식 x87). 왕복 probe.
5. **폴트 합성:** rex86 `Event` → `FaultEvent`(종류 대응은 `MakeFaultEventFromWin32` 기준), INT3 규약,
   TF가 켜져 있으면 `Run(1)` 후 `kSingleStep`.
6. **루프:** `execution_trampoline_emulated.cpp`의 `EnterGuestEmulated`: 진입 상태, 센티넬 복귀 게이트, 복구
   주소 게이트, 슬라이스 반복, `DispatchGuestFault` 호출과 되돌려 쓰기, 슬라이스 경계 `InjectPendingInterrupts`
   (새 `TimerInjectionSite`), `RequestStop`.
7. **메모리:** 런타임 메모리 계획에서 페이지 속성표 만들기. AOT·`native_fast_path` 끄기, `ProtectMemory` 호출자
   전수 확인.
8. **진단:** HLE 경계 trace(두 모델 공통, 환경 변수 opt-in), 버퍼 스왑 사이 단계 수 통계(최종 보고에 한 줄).
9. **실행 확인:** pumpitea `direct`와 `emulated` trace 대조(첫 IRQ0까지 완전 일치), 타이틀 화면 도달, 프레임당
   단계 수 측정. 긴 실행은 필요한 만큼만 한다.
10. **문서:** 작업 로그, `docs/analysis/web-port-frontier.md`와 `docs/TODO.md`를 rex86 기준으로 갱신,
    `ARCHITECTURE.md`의 실행 모델 절, 설계 513에 후속 링크.

완료 기준: Windows `emulated` 빌드로 pumpitea가 타이틀 화면에 도달하고, 첫 IRQ0까지 trace가 `direct`와 일치하며,
프레임당 단계 수가 기록된다. `direct` 빌드는 회귀가 없다.

---

# Work order: an `emulated` execution model on rex86, native first (issue #61)

Design: [20261014-i061](../design/20261014-i061-emulated-execution-model.md)

## rex86 (a separate issue, `../rex86`)

1. `Features::user_mode` (working name, off by default): CPL 3 / IOPL 0 semantics. CLI, STI, HLT, IN, OUT, INS and
   OUTS raise a privileged fault before retiring, and INT n (n≠3) raises #GP before retiring, with EIP at the
   instruction.
2. Unit tests (each instruction faults before retiring, state unchanged), with every existing test and probe passing
   on the five-host CI.
3. Merging and tagging wait for the user. Until then rePIU uses the local rex86 through `FETCHCONTENT_SOURCE_DIR_REX86`.

## rePIU

1. **Build:** `REPIU_EXECUTION_MODEL=emulated`; rex86 through FetchContent (a pinned tag, overridable with a local
   source); `rex86_zydis` instead of `repiu_zydis` where rex86 is present; the build tree `build/win32_x86_emulated`.
2. **Model answers:** `execution_model_emulated.cpp`. Not running guest bytes directly, not a long-mode cache, a zero
   return thunk; whether ticks are injected during the swap wait is decided after checking whether it should match
   `direct`.
3. **Environment:** only host selectors accepted, as flat descriptors (FS base = the TEB); all port I/O and interrupt
   targets declined; RDTSC and CPUID give the host's values.
4. **State conversion:** `CpuState` ↔ `GuestCpuContext` (GPRs, EIP, EFLAGS, selectors, the x87 in FNSAVE layout), with
   a round-trip probe.
5. **Fault synthesis:** rex86 `Event` → `FaultEvent` (kinds mapped as `MakeFaultEventFromWin32` maps them), the INT3
   convention, and `Run(1)` followed by `kSingleStep` when TF is set.
6. **The loop:** `EnterGuestEmulated` in `execution_trampoline_emulated.cpp`: the entry state, the sentinel return
   gate, the recovery-address gates, repeated slices, calling `DispatchGuestFault` and writing back, a slice-boundary
   `InjectPendingInterrupts` (a new `TimerInjectionSite`), and `RequestStop`.
7. **Memory:** build the page attribute table from the runtime memory plan; turn off AOT and `native_fast_path`;
   audit every `ProtectMemory` caller.
8. **Diagnostics:** the HLE boundary trace (common to both models, opt-in by environment variable) and a statistic of
   steps between buffer swaps (one line in the final report).
9. **Running it:** compare pumpitea's `direct` and `emulated` traces (an exact match up to the first IRQ0), reach the
   title screen and measure steps per frame. Long runs only as far as needed.
10. **Documents:** the work log; `docs/analysis/web-port-frontier.md` and `docs/TODO.md` updated to the rex86 state;
    the execution model section of `ARCHITECTURE.md`; a follow-up link in design 513.

Done when the Windows `emulated` build takes pumpitea to its title screen, the trace matches `direct` up to the first
IRQ0, and steps per frame are recorded, with no regression in the `direct` build.
