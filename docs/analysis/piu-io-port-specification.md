# 펌프 잇 업 (PIU) I/O 포트 사양 분석 및 유지 지침
# Pump It Up (PIU) I/O Port Specification Analysis and Maintenance Guidelines

## 1. 하드웨어 개요
## 1. Hardware Overview
초기 도스(DOS) 기반 펌프 잇 업(Pump It Up 1st ~ 3rd) 아케이드 시스템은 안다미로(Andamiro)의 독자적인 **PIU10 ISA PCB(또는 JAMMA I/O 보드)**를 사용합니다. 이 보드는 PC의 ISA 슬롯에 장착되어 시스템 제어, 발판 센서 입력, 코인 입력, 사운드(Yamaha YMZ280B), 딥스위치 세팅 및 93C46 EEPROM 제어 등의 기능을 수행합니다. 
해당 하드웨어는 PC의 I/O 포트 영역 중 `0x02A0`부터 `0x02AF`까지의 주소를 레지스터 맵으로 사용하여 CPU와 통신합니다.

Early DOS-based *Pump It Up* (PIU 1st to 3rd) arcade systems utilize Andamiro's proprietary **PIU10 ISA PCB (or JAMMA I/O board)**. Mounted in the PC's ISA slot, it interfaces with the cabinet's inputs (stage sensors, coins), sound (Yamaha YMZ280B), dip switches, and a 93C46 EEPROM.
This hardware communicates with the CPU via the I/O port address range `0x02A0` through `0x02AF`.

---

## 2. 레지스터 포트 매핑 사양
## 2. Register Port Mapping Specification
MAME(Multiple Arcade Machine Emulator) 프로젝트의 공식 드라이버 [src/mame/misc/xtom3d.cpp](https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d.cpp) 분석에 근거한 상세 포트 사양은 다음과 같습니다.

Based on our analysis of the official MAME driver [src/mame/misc/xtom3d.cpp](https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d.cpp), the detailed port specifications are as follows:

| 포트 주소 (Port) | 데이터 너비 (Width) | 입출력 (Direction) | 기능 및 대상 장치 (Description) | MAME 내부 매핑 함수 / HLE 처리 (MAME Driver / HLE Treatment) |
| :--- | :--- | :--- | :--- | :--- |
| `0x0040 ~ 0x0043` | 8-bit | Read / Write | Intel 8253/8254 PIT (Programmable Interval Timer) 제어 및 카운터 포트 | `unsupported-ignored` 로깅 후 쓰기 무시 및 더미 타이머 에뮬레이션 |
| `0x02A0` | 8/16/32-bit | Read / Write | YMZ280B 칩 오프셋 0. 쓰기=레지스터 번호 선택, 읽기=외부 메모리 readback 래치 | `ymz280b_device` read/write offset 0 |
| `0x02A1` | — | — | 미디코드(16비트 워드의 상위 바이트). 쓰기는 버림, 읽기는 `0xFF` | `umask16(0x00ff)`에 의해 라우팅 제외 |
| `0x02A2` | 8/16/32-bit | Read / Write | YMZ280B 칩 오프셋 1. 쓰기=선택된 레지스터에 데이터, 읽기=상태 레지스터 | `ymz280b_device` read/write offset 1 |
| `0x02A3` | — | — | 미디코드. 쓰기는 버림, 읽기는 `0xFF` | `umask16(0x00ff)`에 의해 라우팅 제외 |
| `0x02A8` | 8-bit | Read | P1 (1인용) 발판 센서 입력 (Up-Left, Up-Right, Center, Down-Left, Down-Right) | `P1 Inputs` read |
| `0x02A9` | 8-bit | Read | 시스템 입력 (코인 센서, 서비스 버튼, 테스트 버튼, 스타트 등) | `System Inputs` read |
| `0x02AA` | 8-bit | Read | P2 (2인용) 발판 센서 입력 | `P2 Inputs` read |
| `0x02AB` | 8-bit | Read | 캐비닛 기타 입력 (보조 코인기, 캐비닛 특수 신호 등) | `Cabinet Inputs` read |
| `0x02AC` | 8-bit | Write | 93C46 Serial EEPROM 제어선 쓰기<br>- Bit 0: Chip Select (CS)<br>- Bit 1: Clock (CLK)<br>- Bit 2: Data Input (DI) | `m_eeprom->clk_write / cs_write / di_write` |
| `0x02AE` | 8-bit | Read | 93C46 Serial EEPROM 데이터 읽기<br>- Bit 0: Data Output (DO) 상태 반환 (읽기 결과: `do_read() \| 0xfe`) | `m_eeprom->do_read() \| 0xfe` |

### 2.1 시스템 입력 비트와 호스트 키 정책 (Task 361)
### 2.1 System input bits and host-key policy (Task 361)

`0x02A9`는 active-low 시스템 입력 포트다. MAME `pumpitup` 정의로 확인된
TEST, COIN1, CLEAR 비트와 rePIU의 SERVICE 호환 매핑은 다음과 같다.
SERVICE `0x40`은 MAME 원본에서 기능이 확정되지 않은 비트이므로, 이 표에서는
사용자 지정 호스트 호환 정책으로 구분한다.

`0x02A9` is the active-low system input port. The TEST, COIN1, and CLEAR bits
confirmed by MAME's `pumpitup` definition and rePIU's SERVICE compatibility
mapping are listed below. MAME does not identify `0x40` as a confirmed service
input, so the table explicitly classifies it as a user-selected host
compatibility policy.

| 마스크 / Mask | rePIU 기능 / Function | 호스트 키 / Host key | 상태 / Status |
|---:|---|---|---|
| `0x02` | TEST | F1 | MAME 정의 확인 / Confirmed by MAME |
| `0x04` | COIN1 | F5 | MAME 정의 확인 / Confirmed by MAME |
| `0x40` | SERVICE | F2 | rePIU 호환 정책 / rePIU compatibility policy |
| `0x80` | CLEAR | F3 | MAME `pumpitup` 정의 확인 / Confirmed by MAME `pumpitup` |

released 상태는 `0xFF`이며, 키를 누르면 해당 마스크가 0으로 내려간다. 따라서
단독 입력의 예상값은 F1=`0xFD`, F2=`0xBF`, F3=`0x7F`, F5=`0xFB`이다.

The released value is `0xFF`; pressing a key clears its mask. Expected values
for individual presses are therefore F1=`0xFD`, F2=`0xBF`, F3=`0x7F`, and
F5=`0xFB`.

### 2.2 2P NumLock OFF 숫자패드 정책 (Task 362)
### 2.2 P2 NumLock-off numeric-keypad policy (Task 362)

2P 발판은 NumLock OFF 상태의 숫자패드 7, 9, 5, 1, 3을 사용한다. Win32는
이 상태의 키를 각각 `VK_HOME`, `VK_PRIOR`, `VK_CLEAR`, `VK_END`, `VK_NEXT`로
보고한다. `VK_CLEAR`는 숫자패드 5의 가상키 이름이며 시스템 입력 CLEAR와는
관련이 없다.

The P2 stage uses numeric-keypad 7, 9, 5, 1, and 3 with NumLock off. Win32
reports these keys as `VK_HOME`, `VK_PRIOR`, `VK_CLEAR`, `VK_END`, and
`VK_NEXT`, respectively. `VK_CLEAR` is the virtual-key name for keypad 5 in
this mode and is unrelated to the system CLEAR input.

| `0x02AA` mask | P2 위치 / Position | NumLock OFF 숫자패드 / Keypad | Win32 가상키 / Virtual key |
|---:|---|---:|---|
| `0x01` | Top-Left | 7 | `VK_HOME` |
| `0x02` | Top-Right | 9 | `VK_PRIOR` |
| `0x04` | Center | 5 | `VK_CLEAR` |
| `0x08` | Bottom-Left | 1 | `VK_END` |
| `0x10` | Bottom-Right | 3 | `VK_NEXT` |

### 2.4 호스트 키 매핑의 출처 (Task 497)
### 2.4 Where the host key mapping now comes from (Task 497)

2.1과 2.2에 적힌 호스트 키는 이제 **기본값**이며, 하드코딩 위치가 아니다. Task 497부터
포트·비트·논리 입력·로그 이름은 `JammaPortBitTable()` 한 표에 있고, 각 논리 입력의 호스트 키는
`cfg/<롬셋 ID>.ini`의 `[Input]` 섹션에서 롬셋별로 바꿀 수 있다. 설정 파일이 없으면 이 문서의
값이 그대로 쓰인다.

이전에는 로그용 비트 이름 표와 스캔 코드가 따로 있어서 서로 어긋나 있었다. 로그는 `0x02`를
`P1-UpRight`로 불렀지만 스캔 코드의 열거 이름은 `kP1Down`이었다. 열거 이름은 실제 비트 의미에
맞춰 `kP1UpRight` 등으로 정정했고, 열거 **순서**는 `JammaInputKeyMask`의 비트 위치이므로 바꾸지
않았다.

설정 키 이름은 이 문서의 발판 위치명을 따른다: `P1_UP_LEFT`, `P1_UP_RIGHT`, `P1_CENTER`,
`P1_DOWN_LEFT`, `P1_DOWN_RIGHT`와 그 P2 대응, 그리고 `TEST`, `SERVICE`, `CLEAR`, `COIN1`.

`COIN2`는 지원하지 않는다. `0x02A9`에서 COIN2에 해당하는 비트가 확정되지 않았고, 남은 비트
(`0x01`, `0x08`, `0x10`, `0x20`) 중 하나를 근거 없이 고르는 것은 원본 하드웨어 동작을 지어내는
일이 된다. 캐비닛 입력 포트 `0x02AB` 역시 아직 스캔하지 않는다. 두 입력을 지원하려면 먼저
게임이 실제로 그 포트를 읽는지와 어느 비트를 보는지를 확인해야 한다.

The host keys in 2.1 and 2.2 are now **defaults**, not hardcoded sites. From Task 497 the
port, bit, logical input, and log name live in one `JammaPortBitTable()`, and each logical
input's host key is configurable per ROM set through the `[Input]` section of
`cfg/<rom-set-id>.ini`. With no config file the values in this document are what apply.

Previously the bit-name table used for logging and the scan code were separate and had
drifted apart: the log called `0x02` `P1-UpRight` while the scan code's enumerator was
`kP1Down`. The enumerators were corrected to the actual bit meanings (`kP1UpRight` and so
on); their **order** is unchanged because `JammaInputKeyMask` uses it as a bit position.

Config key names follow this document's panel positions: `P1_UP_LEFT`, `P1_UP_RIGHT`,
`P1_CENTER`, `P1_DOWN_LEFT`, `P1_DOWN_RIGHT` with their P2 counterparts, plus `TEST`,
`SERVICE`, `CLEAR`, and `COIN1`.

`COIN2` is not supported. Its bit in `0x02A9` is not identified, and picking one of the
remaining bits (`0x01`, `0x08`, `0x10`, `0x20`) without evidence would mean inventing
original hardware behavior. The cabinet input port `0x02AB` is likewise not scanned yet.
Supporting either input requires first confirming that the game reads that port at all and
which bits it examines.

설정 파일 사용법은 [docs/guides/romset-config-files.md](../guides/romset-config-files.md)에
있다.
The configuration file guide is
[docs/guides/romset-config-files.md](../guides/romset-config-files.md).

### 2.3 사운드 창의 ISA 바이트 레인 디코드 (Task 290에서 확정)
### 2.3 ISA byte-lane decode of the sound window (confirmed in Task 290)

MAME io_map은 `map(0x00, 0x03).rw("ymz", read, write).umask16(0x00ff)`입니다. 16비트
버스에서 `umask16(0x00ff)`는 **각 16비트 워드의 하위 바이트만** 라우팅한다는 뜻이므로,
칩이 보이는 오프셋은 바이트 주소의 절반입니다. 짝수 포트만 디코드되고 홀수 포트는
미디코드로 남습니다.

이 규칙이 폭에 무관하게 성립하는 것이 중요합니다. 게스트가 `OUT DX, EAX`(width 4)로
`0x02A0`에 쓰면 4개의 바이트 레인으로 분해되어 레지스터 번호 1회와 데이터 1회가
연속 수행됩니다. 예전에 `unsupported`로 분류하던 `0x02A0 ← 0x00000010`,
`0x02A0 ← 0x00000001`, `0x02A2 ← 0x00000000` 트레이스는 모두 이 형태의 정상적인
YMZ280B 레지스터 프로그래밍이었습니다.

The MAME io_map is `map(0x00, 0x03).rw("ymz", read, write).umask16(0x00ff)`. On a
16-bit bus, `umask16(0x00ff)` routes **only the low byte of each 16-bit word**, so
the chip-visible offset is half the byte address: even ports decode and odd ports
stay undecoded.

The rule holds regardless of access width, which is what matters. A guest
`OUT DX, EAX` (width 4) to `0x02A0` decomposes into four byte lanes and performs one
register-number write followed by one data write. The traces previously classified
as `unsupported` — `0x02A0 <- 0x00000010`, `0x02A0 <- 0x00000001`, and
`0x02A2 <- 0x00000000` — were all ordinary YMZ280B register programming of exactly
this shape.

레지스터 의미와 주소 단위는 [docs/kb/ymz280b-pcm-adpcm-decoder.md](../kb/ymz280b-pcm-adpcm-decoder.md)에
정리했습니다.
Register semantics and address units are documented in
[docs/kb/ymz280b-pcm-adpcm-decoder.md](../kb/ymz280b-pcm-adpcm-decoder.md).

---

## 3. 핵심 HLE 에뮬레이션 전략
## 3. Key HLE Emulation Strategy
1. **포트 입력(Read) 에뮬레이션**:
   - `0x02A8 ~ 0x02AB` (입력): 실기 캐비닛 발판 센서는 기본적으로 풀업 저항 상태인 Active-Low 신호를 가집니다. 따라서 기본값으로 `0xFFU`(또는 `0xFFFFU`)를 채워 반환해야 입력 루프 폴링에서 정상 대기 상태를 유지합니다. 입력 레지스터는 게임이 매 프레임 폴링하므로 EEPROM 읽기와 마찬가지로 **NOP 패치 없이 매번 EIP를 전진시켜 재트랩**해야 합니다. NOP 패치로 원본 IN 명령을 덮어쓰면 최초 1회만 실제 키 상태가 반영되고 이후 press/release가 무시되며, EAX가 idle(`0xFF`)로 갱신되지 않아 무입력이 눌림으로 오인됩니다 (Task 327에서 수정).
   - `0x02AE` (EEPROM 읽기): 게임 엔진은 저장된 설정값을 가져오기 위해 이 포트에서 DO 비트 변화를 감지합니다. `Eeprom93c46` 클래스의 16-bit Microwire 프로토콜 상태 머신을 거쳐 출력되는 데이터 비트를 반환합니다. 이전과 달리 NOP 패치 없이 매번 EIP를 전진시켜 명령을 평가합니다.
   - `0x0040` (PIT 카운터 읽기): 레거시 PC 환경의 타이머 폴링으로 인한 지연을 차단하기 위해, 읽기 요청 시 일정한 간격의 dummy counter 또는 dynamic tick 값을 반환하도록 에뮬레이트합니다.
   - `0x02A0`/`0x02A2` (YMZ280B 읽기): 각각 외부 메모리 readback 래치와 상태 레지스터를 반환합니다. 사운드 창은 JAMMA 입력 범위(`0x02A0`~`0x02AF`) 안에 있으므로 **입력 분기보다 먼저** 가로채야 합니다. 그렇지 않으면 입력 폴백이 항상 `0xFF`를 돌려주어 상태 레지스터가 무의미해집니다.
2. **포트 출력(Write) 에뮬레이션**:
   - `0x02A0`/`0x02A2` (YMZ280B 쓰기): 바이트 레인으로 분해해 레지스터 선택과 데이터 기록으로 전달합니다. **절대 NOP 패치하지 않습니다.** 사운드 레지스터는 재생마다 수십~수백 회 다시 프로그래밍되므로, 최초 1회 후 원본 `OUT`을 NOP으로 덮으면 그 시점 이후 영구 무음이 됩니다. EEPROM·JAMMA 경로와 같은 이유로 EIP만 전진시키고 매번 재트랩합니다 (Task 290).
   - `0x02AC` (EEPROM 쓰기): EEPROM에 전송하는 제어 신호(CS, CLK, DI)를 `Eeprom93c46` 상태 머신으로 전달하여 `nvram/<롬셋 ID>/eeprom.dat` 파일에 설정값을 지속적으로 저장/반영합니다(Task 498 이전에는 모든 롬셋이 작업 디렉터리의 `eeprom.dat` 하나를 공유했습니다). NOP 패치 없이 매번 트랩되어 상태를 갱신합니다.
   - `0x0043` (PIT 제어 쓰기): PC 스피커 톤 생성 및 타이머 주파수 조정을 위해 시스템 코드가 수행하는 PIT 제어 쓰기는 에뮬레이터 구동 안정성을 위해 `unsupported-ignored` 로그 기록을 남긴 뒤 무시 처리합니다.

1. **Port Read Emulation**:
   - `0x02A8 ~ 0x02AB` (Inputs): Real cabinet sensors use active-low signals. Returning default pulled-up values like `0xFFU` (or `0xFFFFU`) prevents hang on sensor loops. Because the game polls these registers every frame, the input read — like the EEPROM read — must **advance EIP and re-trap each time rather than NOP-patch** the guest IN. NOP-patching latches only the first sample, ignores later press/release transitions, and leaves EAX un-refreshed so idle is misread as pressed (fixed in Task 327).
   - `0x02AE` (EEPROM Read): The engine queries this port to fetch persistent configurations. It returns the data bit outputted from the `Eeprom93c46` 16-bit Microwire protocol state machine. The instruction is evaluated dynamically without NOP patching by manually advancing EIP.
   - `0x0040` (PIT Counter Read): To prevent hang during timer calibration polling, return dummy ticks or dynamic values.
   - `0x02A0`/`0x02A2` (YMZ280B reads): return the external memory readback latch and the status register respectively. The sound window sits inside the JAMMA input range (`0x02A0`–`0x02AF`), so it must be intercepted **before** the input branch; otherwise the input fallback answers `0xFF` and the status register becomes meaningless.
2. **Port Write Emulation**:
   - `0x02A0`/`0x02A2` (YMZ280B writes): decomposed into byte lanes and forwarded as register select and register data. **Never NOP-patched.** Sound registers are reprogrammed dozens to hundreds of times per playback, so overwriting the original `OUT` with NOPs after the first execution means permanent silence from that point on. As on the EEPROM and JAMMA paths, EIP advances and the instruction re-traps each time (Task 290).
   - `0x02AC` (EEPROM Write): Writes to EEPROM control lines (CS, CLK, DI) are routed to the `Eeprom93c46` state machine, which persists configurations into `nvram/<rom-set-id>/eeprom.dat` (before Task 498 every ROM set shared one `eeprom.dat` in the working directory). It is continuously trapped without NOP patching.
   - `0x0043` (PIT Control Write): Hardware-level timer rate initialization commands (e.g., `out 0x43, al`) are logged as `unsupported-ignored` and safely bypassed.

---

## 4. 추가 관찰 시 사양 업데이트 및 관리 지침
## 4. Maintenance and Update Guidelines for Future Observations

새로운 포트 번지가 발견되거나 기존 매핑의 다른 사용 사례가 포착되면 아래 지침에 따라 사양을 지속 업데이트해야 합니다.

When new I/O ports or mapping variations are identified, the specification must be updated according to the following guidelines:

1. **바이너리/로더 로그 교차 검증**:
   - 실행 중 `unsupported port I/O` 오류가 발생하면, 즉시 예외 시점의 `EIP` 바이트 창에서 명령어 크기(8비트: 1바이트, 16/32비트: 2바이트) 및 대상 포트 번지(`DX` 레지스터)를 확인하십시오.
2. **문서 동기화**:
   - 확인된 포트 주소와 성격(Read/Write, Bit Map)을 이 문서의 2번 섹션 테이블에 누적하여 추가하십시오.
   - 외부 아케이드 하드웨어 레퍼런스(예: MAME 드라이버, `piutools`/`pumptools` 후킹 저장소)의 실장 사양 출처 링크를 명시하십시오.
3. **HLE 라우터 기능 추가**:
   - `HandlePortIoInstruction`에 해당 포트의 에뮬레이션 계약(더미 반환값 및 EIP 오프셋)을 구현하고, `IsPortIoTraceCandidate` Allow-list에 포트를 등록하십시오.

1. **Cross-validate Loader Logs**:
   - If an `unsupported port I/O` error occurs, trace the instruction size (1 byte for 8-bit, 2 bytes for 16/32-bit) and the target port (`DX` register) from the EIP window immediately.
2. **Synchronize Documentation**:
   - Add new port addresses and details (read/write registers, bit map) to the table in Section 2.
   - Reference authoritative sources (e.g. MAME source, `piutools`/`pumptools` hook repos).
3. **Extend HLE Router**:
   - Implement the emulation contract for the new port in `HandlePortIoInstruction` and add the target port to the allow-list in `IsPortIoTraceCandidate`.

## 5. 입력 포트 읽기 비용과 타이틀별 빈도 (Task 402 측정)

### 확인됨: 게스트는 입력 포트 읽기를 지연 수단으로 쓴다

pumpit3의 폴링 루틴 `0x0301DB10`은 포트 `0x02A8`을 **200회 연속으로 읽고 값을 전부
버립니다**(매 반복 `sub eax,eax`). 읽은 데이터가 목적이 아니라 멀티플렉서 strobe 사이의
settle 시간을 버는 것이 목적입니다. 실기에서는 ISA 버스 사이클 한 번이 곧 지연이므로
이것이 자연스러운 관용구입니다.

이 루프는 반복 횟수가 200으로 **고정**이며, `INT 21h AH=2Ch` 지연 루틴과 달리
자기 보정되지 않습니다. 따라서 호출당 비용을 줄이면 wall time이 그대로 줄어듭니다.

### 확인됨: 호출당 비용의 근인은 `ReadJammaPort8`의 전수 키 조회

`src/platform/win32/io/port_io_emulator.cpp`의 `ReadJammaPort8`은 포트 **1바이트마다**
`GetAsyncKeyState`를 최대 5회 호출합니다. `0x02A8`의 16비트 읽기는 2바이트를 합성하므로
`in ax,dx` 한 번에 최대 **10회**입니다.

측정된 핸들러 본체 비용은 호출당 약 16,000~29,000 cycle이며, 이는 Glide gate 호출
(약 78,000 cycle)의 1/3 수준입니다. 포트 읽기 하나가 그만큼 비쌀 이유는 없습니다.

### 확인됨: 빈도는 타이틀마다 크게 다르다

각 45초, `aot-dbt`, `REPIU_EXECUTION_TIME_PROFILE=1` 기준입니다.

| 타겟 | 프레임 | port I/O 호출 | 초당 | 배수 | port-io 비중 | Glide gate 비중 | 호출당 cycle |
|---|---:|---:|---:|---:|---:|---:|---:|
| pumpit1 | 2,222 | 25,091 | 558 | 1.0x | 0.42% | 57.20% | 27,615 |
| pumpit2 | 1,985 | 40,162 | 892 | 1.6x | 0.38% | 35.77% | 15,922 |
| **pumpit3** | **1,026** | **1,846,040** | **41,023** | **73.6x** | **30.53%** | 12.64% | 29,395 |

**호출당 비용은 세 타이틀이 같은 자릿수이므로, pumpit3의 큰 비중은 비싼 호출이 아니라
호출 횟수에서 옵니다.** pumpit3의 초당 41,023회는 200회 루프 기준 초당 약 205회 폴링이며,
게스트가 프로그램한 PIT 240Hz와 같은 자릿수입니다.

**정적 개수로는 이 차이를 알 수 없습니다.** 200회 지연 루프(`cmp ebx,0xC8; jl`)는
pumpit1 4곳(`0x44E03` `0x450C7` `0x45377` `0x45627`), pumpit2 4곳, pumpit3 1곳
(`0x28D24`)으로, 오히려 pumpit1/pumpit2에 더 많습니다.

### 유지 지침

입력 포트 읽기 비용을 줄일 때 지켜야 할 제약은 Task 327이 확정한 것입니다: **매 폴링마다
EIP를 전진시켜 재트랩해야 하며 NOP 패치로 대체하면 안 됩니다.** 최초 1회만 반영되면
press/release 전이가 무시되고 무입력이 눌림으로 오인됩니다.

이 제약과 양립하는 방향은 트랩 자체를 없애는 것이 아니라 **호스트 키보드 전수 조회를
매 읽기마다 하지 않는 것**입니다. 입력 스냅샷을 프레임 또는 타이머 틱 주기로 갱신하고
포트 읽기는 스냅샷을 소비하면, 스냅샷 주기가 게스트 폴링 주기보다 촘촘한 한 전이는
보존됩니다.

### 미확정

- 호출당 약 29,000 cycle 중 `GetAsyncKeyState`가 차지하는 비중은 직접 계측하지
  않았습니다. `RecordPortIo`가 `const std::string&`를 받아 호출마다 문자열을 만드는
  비용도 분해되지 않았습니다.
- 위 세 실행은 **같은 장면이 아닙니다.** 각 타이틀의 부팅 후 45초일 뿐이며, Tasks
  363~368이 확인했듯 장면이 달라지면 같은 비용 집합도 몇 배씩 움직입니다.
- pumpit1의 200회 루프 4곳이 런타임에 실행되기는 하는지, 아니면 낮은 빈도가 그 경로를
  타지 않기 때문인지 분해하지 않았습니다.

## 5. Input-port read cost and per-title frequency (measured in Task 402)

### Confirmed: the guest uses input-port reads as a delay

pumpit3's polling routine at `0x0301DB10` reads port `0x02A8` **200 times in a row and
discards every value** (`sub eax,eax` each iteration). The data is not the point; the settle
time between multiplexer strobes is. On real hardware one ISA bus cycle *is* the delay, which
makes this a natural idiom.

The loop has a **fixed** 200 iterations and, unlike the `INT 21h AH=2Ch` delay routine, is
not self-calibrating, so reducing per-call cost reduces wall time directly.

### Confirmed: the per-call cost comes from a full key scan in `ReadJammaPort8`

`ReadJammaPort8` in `src/platform/win32/io/port_io_emulator.cpp` calls `GetAsyncKeyState` up
to five times **per port byte**. A 16-bit read at `0x02A8` composes two bytes, so a single
`in ax,dx` performs up to **ten**.

Measured handler-body cost is roughly 16,000-29,000 cycles per call — about a third of a
Glide gate call (~78,000 cycles). A port read has no reason to be that expensive.

### Confirmed: frequency differs sharply by title

45 seconds each, `aot-dbt`, `REPIU_EXECUTION_TIME_PROFILE=1`:

| Target | Frames | Port I/O calls | Per second | Ratio | Port-io share | Glide gate share | Cycles/call |
|---|---:|---:|---:|---:|---:|---:|---:|
| pumpit1 | 2,222 | 25,091 | 558 | 1.0x | 0.42% | 57.20% | 27,615 |
| pumpit2 | 1,985 | 40,162 | 892 | 1.6x | 0.38% | 35.77% | 15,922 |
| **pumpit3** | **1,026** | **1,846,040** | **41,023** | **73.6x** | **30.53%** | 12.64% | 29,395 |

**Per-call cost is the same order across titles, so pumpit3's large share comes from call
count, not from more expensive calls.** Its 41,023 reads per second is about 205 polls per
second at 200 reads each, the same order as the guest-programmed 240 Hz PIT rate.

**Static counts cannot reveal this difference.** The 200-iteration delay loop
(`cmp ebx,0xC8; jl`) appears four times in pumpit1 (`0x44E03`, `0x450C7`, `0x45377`,
`0x45627`), four times in pumpit2, and once in pumpit3 (`0x28D24`) — more in pumpit1 and
pumpit2, not fewer.

### Maintenance guidance

The constraint to preserve when reducing input-port read cost is the one Task 327 established:
**advance EIP and re-trap on every poll; never NOP-patch the guest `IN`.** Latching only the
first sample ignores later press/release transitions and misreads idle as pressed.

The direction compatible with that constraint is not removing the trap but **not rescanning
the host keyboard on every read**: refresh an input snapshot per frame or timer tick and have
port reads consume it. Transitions are preserved as long as the snapshot cadence is finer than
the guest's polling cadence.

### Unresolved

- How much of the ~29,000 cycles per call is `GetAsyncKeyState` was not measured directly,
  nor was the cost of `RecordPortIo` taking a `const std::string&` and constructing a string
  per call.
- The three runs above are **not the same scene** — each is a title's first 45 seconds — and
  Tasks 363-368 showed scene choice moves these figures several-fold.
- Whether pumpit1's four 200-iteration loops execute at runtime at all, or whether its low
  frequency means that path is not taken, was not decomposed.

### 5.1 확인됨: 호출당 비용은 사실상 전부 `GetAsyncKeyState` (Task 403 분해)

Task 402가 미확정으로 남긴 "호출당 약 29,000 cycle 중 `GetAsyncKeyState` 비중"을
직접 계측했습니다. `ReadJammaPort8` 루프에 rdtsc 구간을 두고 `GetAsyncKeyState` 호출을
셌습니다(pumpit3, 45초, `aot-dbt`).

| 항목 | 값 | 비중 |
|---|---:|---:|
| wall cycles | 166,599,116,119 | 100% |
| port I/O 핸들러 본체 | 51,474,994,231 | 30.90% of wall |
| └ JAMMA scan 루프 | 51,070,572,912 | **99.21% of port-io body** |
| └ → wall 대비 | | **30.65%** |
| scan 수 | 1,868,147 | 초당 41,514 |
| `GetAsyncKeyState` 호출 | 16,776,443 | 초당 372,810 |
| scan당 key query | **8.98** | |
| **key query당 cycle** | **3,044** | 약 0.82µs |

**9 × 3,044 = 27,396 cycle이고 실측 scan당 27,337 cycle이므로, key query가 scan 비용의
100.2%를 설명합니다.** 디코드, `RecordPortIo`, 문자열 생성 등 나머지는 측정 오차 범위
안입니다.

따라서 **port I/O 비용의 근인은 `GetAsyncKeyState` 하나**이며, scan 비용을 0으로 만들 때의
상한은 `1 / (1 - 0.3065) = 1.44배`입니다. 커널 예외 왕복은 Task 327 규칙상 그대로
남으므로 상한에 포함되지 않습니다.

### 5.2 정확도를 유지하면서 호출을 줄이는 조건

**게스트가 관측할 수 있는 주기가 상한입니다.** pumpit3는 초당 41,514회 읽지만 그중
200회가 하나의 폴링 안에서 값을 버리는 지연이므로, 실제 폴링은 초당 약 208회
(약 4.8ms)입니다. 한 폴링 안의 200회 읽기는 약 1.5ms에 걸쳐 있고 그 사이 사람 손가락의
전이는 일어나지 않습니다.

따라서 **스냅샷 갱신 주기가 게스트 폴링 주기보다 촘촘하면 게스트가 볼 수 있었던 전이는
하나도 잃지 않습니다.** 갱신 주기 1ms 기준:

| | 현재 | 스냅샷 1ms |
|---|---:|---:|
| `GetAsyncKeyState` 초당 호출 | 372,810 | 약 14,000 (키 14개 × 1,000) |
| 감소 | | 약 26배 |
| 추가 입력 지연 최악 | 0 | 1ms (게스트 폴링 4.8ms의 1/5) |

Task 327 제약은 그대로 지켜집니다. **트랩을 없애는 것이 아니라 트랩 안에서 호스트
키보드를 전수 조회하지 않는 것**이므로, 게스트 `IN`은 매번 실행되고 EIP는 매번
전진하며 NOP 패치도 하지 않습니다.

캐시해도 안전한 근거가 하나 더 있습니다. 현재 코드는 `GetAsyncKeyState`의 반환에서
`0x8000`(현재 눌림 상태)만 사용하고 **"마지막 호출 이후 눌린 적 있음" 비트(`0x0001`)는
쓰지 않습니다.** 그 비트는 호출이 소비하는 상태이므로 캐시하면 의미가 달라지지만,
쓰지 않으므로 호출 횟수를 줄여도 관측 의미가 바뀌지 않습니다.

**캐시 대상은 JAMMA 키 스캔으로 한정합니다.** EEPROM 포트와 사운드 포트는 상태를 가지며
읽기마다 부작용이 있으므로 캐시하면 안 됩니다.

### 5.1 Confirmed: per-call cost is essentially all `GetAsyncKeyState` (Task 403)

The item Task 402 left unresolved — how much of the ~29,000 cycles per call is
`GetAsyncKeyState` — was measured directly by timing the `ReadJammaPort8` loop with rdtsc
and counting the calls (pumpit3, 45 seconds, `aot-dbt`).

| Metric | Value | Share |
|---|---:|---:|
| Wall cycles | 166,599,116,119 | 100% |
| Port I/O handler body | 51,474,994,231 | 30.90% of wall |
| ��� JAMMA scan loop | 51,070,572,912 | **99.21% of port-io body** |
| └ against wall | | **30.65%** |
| Scans | 1,868,147 | 41,514/s |
| `GetAsyncKeyState` calls | 16,776,443 | 372,810/s |
| Key queries per scan | **8.98** | |
| **Cycles per key query** | **3,044** | ~0.82 µs |

**9 × 3,044 = 27,396 cycles against a measured 27,337 per scan, so key queries account for
100.2% of the scan cost.** Decode, `RecordPortIo`, and string construction fall within
measurement noise.

The root cause of port I/O cost is therefore `GetAsyncKeyState` alone, and driving the scan
cost to zero caps the gain at `1 / (1 - 0.3065) = 1.44x`. The kernel exception round trip
stays, since the Task 327 rule keeps the trap.

### 5.2 Conditions for calling it less without losing accuracy

**The guest's own observation period is the ceiling.** pumpit3 issues 41,514 reads per second,
but 200 of every batch are a value-discarding delay, so it actually polls about 208 times per
second (~4.8 ms). The 200 reads in one poll span roughly 1.5 ms, within which no human key
transition occurs.

**A snapshot refreshed faster than the guest polls therefore loses no transition the guest
could have observed.** At a 1 ms refresh:

| | Now | 1 ms snapshot |
|---|---:|---:|
| `GetAsyncKeyState` per second | 372,810 | ~14,000 (14 keys × 1,000) |
| Reduction | | ~26x |
| Worst-case added input latency | 0 | 1 ms (a fifth of the guest's 4.8 ms poll) |

The Task 327 constraint still holds: this does not remove the trap, it stops the trap from
rescanning the host keyboard, so the guest `IN` still executes, EIP still advances, and
nothing is NOP-patched.

One more reason caching is safe: the current code uses only the `0x8000` bit (current physical
state) and **never the `0x0001` "pressed since last call" bit**. That bit is state a call
consumes, so caching would change its meaning — but it is unused, so reducing call frequency
does not change what is observed.

**Only the JAMMA key scan may be cached.** The EEPROM and sound ports carry state with
per-read side effects and must not be.

### 5.3 스냅샷 도입 결과 (Task 403 구현 + A/B)

`ReadJammaPort8`을 `ScanJammaPort8`(실제 조회)과 스냅샷 조회로 분리하고, 갱신 주기를
기본 500µs로 제한했습니다. 끄기 스위치 `REPIU_JAMMA_SNAPSHOT`은 issue #24에서 없앴고,
주기 `REPIU_JAMMA_SNAPSHOT_US=0`이면 매 읽기 조회가 됩니다.

pumpit3 45초 각 6회, `REPIU_EEPROM_PATH`로 EEPROM을 격리한 A/B입니다.

| | 렌더 도달 | 프레임(정렬) | port-io 비중 중앙값 | key query/s 중앙값 |
|---|---|---|---:|---:|
| off | 4/6 | 0, 0, 867, 867, 867, 1106 | 22.40% | 285,425 |
| on | 3/6 | 0, 0, 0, 867, 1340, 1389 | **2.75%** | **20,080** |

**확인됨: 비용 제거.** key query **14.2배** 감소, port-io wall 비중 22.40% → 2.75%.

**미확정: 프레임 개선.** 렌더 도달 실행만의 중앙값은 867 → 1,340이지만 표본이 6회씩이고
결과가 0/약1,300으로 양극화되어 있습니다. 특히 **867이 여러 실행에서 정확히 반복**되므로
이 값은 처리량 상한이 아니라 장면 경계일 가능성이 높고, 그렇다면 프레임 수는 throughput
지표가 아닙니다.

**미확정: 렌더링 도달 자체가 비결정적.** 두 arm 모두 6회 중 2~3회가 45초 안에 렌더링에
도달하지 못했습니다. 스냅샷과 무관한 기존 문제이며, 해소 전에는 프레임 기반 성능 판정을
신뢰할 수 없습니다.

**회귀 없음.** pumpit1 2,251 프레임(이전 2,222), pumpit2 2,204(이전 1,985).

**측정 절차 주의.** `eeprom.dat`는 git에 추적되지 않는 영속 상태입니다. 격리하지 않은
첫 A/B는 무효였고, 매 실행마다 고정 fixture를 복사해 `REPIU_EEPROM_PATH`로 지정한 뒤에야
신호가 나왔습니다. `scripts/benchmark_*.ps1`이 쓰는 방식과 같습니다.

### 5.3 Snapshot result (Task 403 implementation and A/B)

`ReadJammaPort8` was split into `ScanJammaPort8` (the real query) and a snapshot lookup with a
default 500 µs refresh bound. The `REPIU_JAMMA_SNAPSHOT` kill switch went in issue #24;
`REPIU_JAMMA_SNAPSHOT_US=0` gives per-read querying.

Six 45-second pumpit3 runs per arm with the EEPROM isolated through `REPIU_EEPROM_PATH`:

| | Reached rendering | Frames (sorted) | Port-io share median | Key queries/s median |
|---|---|---|---:|---:|
| off | 4/6 | 0, 0, 867, 867, 867, 1106 | 22.40% | 285,425 |
| on | 3/6 | 0, 0, 0, 867, 1340, 1389 | **2.75%** | **20,080** |

**Confirmed: the cost is removed.** Key queries fell **14.2x** and the port-io wall share went
from 22.40% to 2.75%.

**Unresolved: whether frames improve.** The median among rendering runs moves 867 to 1,340, but
with six runs per arm, outcomes polarised at 0 or ~1,300, and the value **867 recurring exactly**
across runs — which suggests a scene boundary rather than a throughput ceiling, making frame
count a "how far did it get" measure rather than throughput.

**Unresolved: reaching rendering is itself nondeterministic.** Two to three of six runs in both
arms never rendered within 45 seconds. This predates the snapshot, and until it is understood,
frame-based performance judgements cannot be trusted.

**No regression.** pumpit1 rendered 2,251 frames (2,222 before) and pumpit2 2,204 (1,985 before).

**Measurement caveat.** `eeprom.dat` is untracked persistent state. The first A/B run without
isolating it was invalid; a usable signal appeared only after copying a fixed fixture per run
and pointing `REPIU_EEPROM_PATH` at it, the method `scripts/benchmark_*.ps1` already use.

## 6. 별도 PIU10 flash/MP3/CAT702 보드 (Task 451)

### 한국어

**확인됨: 장치 구분.** MAME의
[`xtom3d_piu10.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d_piu10.cpp)는
`0x02D0..0x02DF`를 별도 ISA16 PIU10 장치로 설치합니다. 이는
[`xtom3d.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d.cpp)의
`0x02A0..0x02AF` JAMMA/YMZ280B I/O·sound 보드와 동일 장치가 아닙니다.

| port | 방향 | 확인된 기능 |
|---:|---|---|
| `0x02D0` | write16 | address bits 0..7 |
| `0x02D2` | write16 | address bits 8..15 |
| `0x02D4` | write16 | address bits 16..19, destination bits 0..3 |
| `0x02D6` | write16 | destination bits 4..11 |
| `0x02DA` | read/write16 | destination별 flash data, CAT702/DAC serial, MP3 data/status |
| `0x02DC` | write16 | bit 3 flash unlock/읽기 주소 자동 증가 |

목적지 `0x008`의 `0x02DA` read bit는 CAT702 data-out=5, MPEG frame-sync=2,
send-ready=1, decoder-demand=0입니다. 목적지 `0x010`의 write bit는 CAT702
data-in=5, clock=4, select=3이며 DAC I2C SDA/SCL=1/0입니다. CAT702의 PIU 변형은
select가 low로 바뀔 때 `0xFC` 상태에 초기 transform을 적용하고, 선택 중 clock rising
edge마다 입력 bit에 따른 linear transform과 다음 출력 bit를 계산합니다. 근거 구현은
MAME [`cat702.cpp`](https://github.com/mamedev/mame/blob/master/src/devices/machine/cat702.cpp)입니다.

**확인됨: pumpito 실패와 수정 후 실행.** 수정 전 10초 미만 실행은 guest
`0x0402106D`의 `IN AX,DX`, DX=`0x02DA`에서 끝났고 port I/O는 input/output/handled/unhandled
`5/28/28/5`였습니다. Task 451 후 3초 실행은 예외 없이 timeout까지 진행했고
`1/31/32/0`이었습니다. 마지막 접근은 `0x0402108A`의 `OUT` `0x02DA <- 0xFFFF`이며
`emulated-piu10-write`로 처리됐습니다. 10초 실행에서는 `BGA\\00.DAT` open/read까지
진행했습니다.

**미확정:** 실제 MP3 decode/audio 출력, 화면 렌더링, 입력, 전체 gameplay는 이번
검증 범위가 아닙니다. 현재 MP3 status는 reset-ready 신호만 모델링합니다.

**확인됨: target 범위(Task 452).** rePIU는 이 별도 장치를 `pumpito`, `pumpitc`,
`pumpitpc`, `pumpite`에서만 활성화합니다. `pumpit1`, `pumpit2`, `pumpit3`에서는
비활성화되며, 이 target들의 YMZ280B sample ROM 경로는 해당 capability와 독립적입니다.

**확인됨: CAT702 독립 capability(Task 469).** `enable_cat702`은 PIU10 보드 전체와
별도로 target profile에서 선택합니다. false이면 `<target>.cat702`를 추출하지 않고
data-out bit 5를 0으로 반환하며 data/clock/select 쓰기를 무시합니다. 같은 포트의
flash, MP3와 DAC 경로는 유지됩니다. 현재 내장 profile은 기존 동작을 보존하도록
`pumpito`, `pumpitc`, `pumpitpc`, `pumpite`에서 true입니다.

**확인됨: JAMMA target 범위(Task 453).** `0x02A0..0x02AF` JAMMA/YMZ280B/EEPROM
보드는 `enable_piu_jamma_board` capability로 제어합니다. 현재 `pumpit1`, `pumpit2`,
`pumpit3`, `pumpito`, `pumpitc`, `pumpitpc`, `pumpite`에서만 활성화되며
`dos4gw_hello`, `piu_1st`, direct executable에서는 비활성화됩니다. 이는 별도
`0x02D0..0x02DF` PIU10 flash/CAT702 capability의 범위를 변경하지 않습니다.

### English

**Confirmed: separate devices.** MAME installs `0x02D0..0x02DF` as a separate ISA16 PIU10
device in `xtom3d_piu10.cpp`. It is not the JAMMA/YMZ280B I/O and sound board at
`0x02A0..0x02AF` in `xtom3d.cpp`.

The port table above assembles a 20-bit address and 12-bit destination through `0x02D0..0x02D6`;
`0x02DA` carries destination-specific flash data, CAT702/DAC serial lines, or MP3 data/status;
bit 3 of `0x02DC` controls flash unlock and read-address increment. At destination `0x008`, read
bits 5/2/1/0 are CAT702 data-out, MPEG frame-sync, send-ready, and decoder-demand. At destination
`0x010`, write bits 5/4/3 drive CAT702 data-in/clock/select, while bits 1/0 drive DAC I2C SDA/SCL.
The CAT702 PIU variant initializes its transformed `0xFC` state on select-low and advances its
linear transform/output bit on each selected rising clock edge.

**Confirmed: pumpito failure and post-fix execution.** Before the fix, execution stopped at guest
`0x0402106D`, `IN AX,DX`, DX=`0x02DA`, with port I/O input/output/handled/unhandled
`5/28/28/5`. After Task 451, a three-second run reached its time limit without an exception and
reported `1/31/32/0`. Its last access was `OUT 0x02DA <- 0xFFFF` at `0x0402108A`, handled as
`emulated-piu10-write`. A ten-second run progressed through opening and reading `BGA\\00.DAT`.

**Unresolved:** actual MP3 decoding/audio output, rendering, input, and complete gameplay are not
verified by this task. The current MP3 status model reports reset-ready signals only.

**Confirmed target scope (Task 452):** rePIU enables this separate device only for `pumpito`,
`pumpitc`, `pumpitpc`, and `pumpite`. It remains disabled for `pumpit1`, `pumpit2`, and
`pumpit3`; their YMZ280B sample-ROM path is independent of this capability.

**Confirmed independent CAT702 capability (Task 469):** target profiles select `enable_cat702`
independently from the complete PIU10 board. When false, setup does not extract
`<target>.cat702`, data-out bit 5 reads zero, and data/clock/select writes are ignored. Flash,
MP3, and DAC paths on the same port remain active. The built-in profiles preserve previous
behavior by setting it true for `pumpito`, `pumpitc`, `pumpitpc`, and `pumpite`.

**Confirmed JAMMA target scope (Task 453):** the `0x02A0..0x02AF` JAMMA/YMZ280B/EEPROM
board is controlled by `enable_piu_jamma_board`. It is enabled only for `pumpit1`, `pumpit2`,
`pumpit3`, `pumpito`, `pumpitc`, `pumpitpc`, and `pumpite`, and disabled for
`dos4gw_hello`, `piu_1st`, and direct executables. This does not change the scope of the
separate `0x02D0..0x02DF` PIU10 flash/CAT702 capability.

## 7. PIU10 MAS3507D MP3 전송과 HLE (Task 454)

### 한국어

**확인됨:** MAME
[`xtom3d_piu10.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d_piu10.cpp)는 목적지 `0x008`의 `0x02DA` write를
MAS3507D `sid_w`에 연결합니다. pumpito의 종료 지점 `OUT DX,AL`, DX=`0x02DA`는 따라서
지원되지 않는 폭이 아니라 8-bit PIO-DMA MP3 data write입니다. Micronas의 MAS3507D F10
[supplement](https://floe.butterbrot.org/matrix/hacking/limp/docs/mas3507d_3pds.pdf)도 이 revision에 8-bit PIO-DMA 입력과 demand handshake가 있음을 설명합니다.

**확인됨:** mount된 `AUDIO/01.AUD`부터 `04.AUD`의 첫 32 byte는 ID3 또는 MPEG frame
sync로 시작하지 않습니다. host의 `.AUD` 경로를 직접 decoder에 넘길 수 없으며, 원본 게임이
읽고 변환한 뒤 PIU10 port로 내보내는 byte stream이 HLE 입력 경계입니다.

**확인됨:** Task 454 적용 후 제한 실행은 port I/O
input/output/handled/unhandled=`112955/264794/377749/0`으로 종료되어 기존
`unsupported-piu10-width`가 재현되지 않았습니다. SDL3_mixer `dr_mp3`는 guest가 전달한
50,585-byte 구간을 decode하고 playback을 시작했습니다. 그 전에 관찰된 두 개의 2-byte
전송은 MPEG frame header보다 짧으므로 decode 대상에서 제외합니다.

**Task 455에서 대체됨:** ready/demand-high 모델은 유지하지만 30 ms idle framing은 더 이상
사용하지 않습니다. 아래의 증분 frame decoder가 이 Task 454 정책을 대체합니다.

### English

**Confirmed:** MAME's
[`xtom3d_piu10.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d_piu10.cpp) connects a destination-`0x008` write at `0x02DA`
to MAS3507D `sid_w`. The pumpito termination at `OUT DX,AL`, DX=`0x02DA`, is therefore an
eight-bit PIO-DMA MP3 data write, not an unsupported access width. Micronas's MAS3507D F10
[supplement](https://floe.butterbrot.org/matrix/hacking/limp/docs/mas3507d_3pds.pdf) also documents the revision's eight-bit PIO-DMA input and demand handshake.

**Confirmed:** The first 32 bytes of mounted `AUDIO/01.AUD` through `04.AUD` do not start
with ID3 or an MPEG frame sync. The host `.AUD` path cannot be handed directly to a decoder;
the HLE input boundary is the byte stream that the original game reads, transforms, and emits
through the PIU10 port.

**Confirmed:** After Task 454, a timeout-bounded pumpito run reported port I/O
input/output/handled/unhandled=`112955/264794/377749/0`; the old
`unsupported-piu10-width` did not recur. SDL3_mixer `dr_mp3` decoded a 50,585-byte guest
segment and started playback. Two earlier two-byte transfers are shorter than an MPEG frame
header and are filtered before decoding.

**Superseded by Task 455:** The ready/demand-high model remains, but 30 ms idle framing is no
longer used. The incremental frame decoder below replaces this Task 454 policy.

## 8. 증분 MP3 시작 시점 (Task 455)

### 한국어

**확인됨:** SDL3_mixer 3.2.0 `MIX_SetTrackIOStream`은 전체 data를 seek할 수 있어야 하며,
`decoder_drmp3.c` 초기화는 frame count와 seek table을 얻기 위해 전체 입력을 scan합니다.
따라서 끝나지 않은 성장형 IO stream을 직접 연결해도 조기 재생이 되지 않습니다.

**확인됨:** Task 455는 Task 454의 30 ms idle/전체 구간 predecode를 대체합니다. 공용 MPEG
header parser가 version, layer, bitrate, sample rate, padding으로 frame 길이를 검증하고,
Win32 backend는 완성된 4 frame batch만 SDL3_mixer decoder에 전달합니다. PCM은 하나의
지속적인 SDL audio stream/MIX track에 누적됩니다.

**확인됨:** 동일한 pumpito 제한 실행에서 증분 playback은 2,055 guest byte 수신 시점에
4개 MPEG frame으로 시작했습니다. 이전 방식의 관찰값 50,585 byte보다 48,530 byte 앞이며,
port I/O input/output/handled/unhandled는 `112897/235038/347935/0`입니다.

**미확정:** 실제 cabinet의 MAS3507D demand와 frame-sync timing은 아직 고정 high입니다.
guest 공급이 PCM 소비보다 늦으면 backend가 track을 유지하기 위해 silence를 삽입합니다.
장시간 청취에서 underrun 빈도와 곡 전환의 연속성을 확인해야 합니다.

### English

**Confirmed:** SDL3_mixer 3.2.0 `MIX_SetTrackIOStream` requires seeking over the complete data,
and `decoder_drmp3.c` initialization scans the whole input for frame counts and a seek table.
Connecting an unfinished growing IO stream therefore does not provide early playback.

**Confirmed:** Task 455 replaces Task 454's 30 ms idle/whole-segment predecode. A shared MPEG
header parser validates frame length from version, layer, bitrate, sample rate, and padding.
The Win32 backend sends only complete four-frame batches to the SDL3_mixer decoder and appends
their PCM to one persistent SDL audio stream and MIX track.

**Confirmed:** In the same timeout-bounded pumpito scenario, incremental playback began from
four MPEG frames after 2,055 guest bytes. This is 48,530 bytes earlier than the previous
50,585-byte observation. Port I/O input/output/handled/unhandled was
`112897/235038/347935/0`.

**Unresolved:** Actual-cabinet MAS3507D demand and frame-sync timing remain fixed high. If guest
supply falls behind PCM consumption, the backend inserts silence to keep the track alive.
Long listening still needs to establish underrun frequency and continuity across song changes.

## 9. Task 455 사용자 검증과 교체 방향 (Task 456)

### 한국어

**확인됨:** 사용자의 실제 플레이 검증에서 Task 455 증분 SDL3_mixer 방식은 음악이 심하게
끊기고 MP3 재생 중 게임 진행이 멈추어 사용할 수 없는 것으로 판정되었습니다. 따라서
2,055 byte 조기 시작 자체는 성공했지만, 재생 연속성과 guest 실행 동시성은 충족하지
못했습니다.

**추정:** 주요 원인은 작은 frame batch마다 SDL3_mixer decoder를 새로 만드는 비용과,
destination `0x008`의 MP3 byte마다 일반 HLE adapter, 계측, 동기화 경로를 통과하는 비용이
guest의 전송 loop를 장시간 점유하는 데 있습니다. underrun 때 silence를 삽입하는 정책도
끊김을 숨기지 못하고 증상을 연장합니다.

**구현됨:** upstream `minimp3`의 하나의 persistent `mp3dec_t`가 4 MiB SPSC 압축 byte
ring에서 완성 frame을 읽고 PCM을 SDL3 audio stream으로 직접 전달합니다. demand는 ring
여유 공간, frame-sync는 성공한 decode마다의 토글입니다. destination `0x008`의 정확한
`OUT DX,AL`은 AOT resolver와 arena port handler의 전용 조기 경로를 사용하며 다른 PIU10
port의 기존 HLE 계약은 유지합니다. MAME 구현은 계약과 처리 순서의 참고 자료로만 사용했고
코드는 포함하지 않았습니다. 상세 설계는
[Task 456 MAS3507D streaming HLE](../design/20260809-456-mas3507d-streaming-hle.md)에 있습니다.

**확인됨:** 제한 pumpito 실행에서 첫 playback은 1,257 guest byte에서 시작했고, 이후 다른
음악과 BGA 자산 로딩까지 진행했습니다. port I/O unhandled는 0이고 실행 예외는 없었습니다.
후속 초기 편차 실행에서는 504-byte 준비 전송만 관찰되어 arena fast path의 본 전송 실측과
장시간 청감 연속성은 아직 사용자 검증이 필요합니다.

### English

**Confirmed:** The user's real-play validation rejected Task 455 because audio stuttered severely
and the game stopped progressing during MP3 playback. Starting after 2,055 bytes succeeded, but
continuous playback and concurrent guest execution did not.

**Inferred:** The primary costs are constructing a new SDL3_mixer decoder for every small frame
batch and routing every destination `0x008` MP3 byte through the generic HLE adapter,
instrumentation, and synchronization path. Inserting silence on underrun prolongs rather than
solves the audible failure.

**Implemented:** One persistent upstream `minimp3` `mp3dec_t` consumes complete frames from a
4 MiB compressed-byte SPSC ring and sends PCM directly to an SDL3 audio stream. Ring capacity
drives demand, and each successful decode toggles frame-sync. Exact destination-`0x008`
`OUT DX,AL` uses an early path in both the AOT resolver and arena port handler while other PIU10
ports retain their existing contract. MAME was used only to reference the contract and processing
order; no MAME code was incorporated. See
[Task 456 MAS3507D streaming HLE](../design/20260809-456-mas3507d-streaming-hle.md).

**Confirmed:** A bounded pumpito run began playback after 1,257 guest bytes and continued to load
later music and BGA assets. Port-I/O unhandled remained zero and no execution exception occurred.
A later startup-variation run observed only the 504-byte preparation transfer, so main-transfer
arena-fast-path measurements and long listening continuity still require user validation.

## 10. MAS3507D FIFO와 pumpito frame 공급 loop (Task 457)

### 한국어

**확인됨:** 사용자 실행의 최종 통계는
`received/dropped/decoded/pcm/starved/ring-high=2282730/0/5465/6295680/210/108`이었습니다.
약 251초 동안 2,282,730 byte를 받았지만 4 MiB ring의 최고 수위는 108 byte에 불과했습니다.
따라서 guest가 곡 전체를 ring에 먼저 채워서 게임 진행이 막힌 것이 아니라, byte마다 발생하는
privileged `OUT` 예외가 약 9 KiB/s로 MP3 소비 속도보다 느려 decoder starvation과 guest thread
점유를 동시에 만들었습니다.

**확인됨:** 원본 `pumpito` image offset `0x212FD`의 `OUT DX,AL` loop는 출력 전에 source
cursor와 현재 frame byte count를 증가시키고, 출력 뒤 `ECX`와 count를 증가시켜 현재 MPEG
frame 길이와 비교합니다. 관련 원본 주소는 source cursor `0x00343420`, available end
`0x00343424`, frame length `0x00343418`, frame count `0x0034341C`, source buffer
`0x00343438`입니다.

**구현됨:** 압축 ring은 4 KiB 물리 용량과 `0xE00` byte 논리 `DEMAND` 한계를 사용합니다.
정확한 `pumpito` loop signature, relocation, 메모리 범위와 frame 상태가 모두 맞을 때만 현재
frame tail을 한 번의 SPSC span write로 옮기고 source cursor, frame count, `ECX`를 건너뛴
원본 byte loop와 같게 갱신합니다. 불일치 또는 공간 부족 시 기존 byte 경로로 복귀하며,
다른 target에는 이 signature를 활성화하지 않습니다. MAME의 `0xE00` 값은 하드웨어 계약
참고에만 사용했고 코드는 포함하지 않았습니다.

**확인됨:** 자동 probe는 `0xE00`에서 demand 해제, 512-byte 소비 후 재설정, high-water
3584와 4-byte frame-tail 계획 및 3-byte 부분 commit 후 `ECX=103`을 검증했습니다. Win32 x86
Debug의 `repiu`와 `repiu_aot_probe` build도 성공했습니다.

**미확정:** 60초와 90초 제한 실행은 startup 편차로 MP3 본 전송 구간에 도달하지 못했으므로
실제 block-HLE 활성화, 장시간 청감, 음악 중 렌더링·입력·장면 전환 동시 진행은 사용자 환경에서
확인해야 합니다.

### English

**Confirmed:** The user's final runtime statistics were
`received/dropped/decoded/pcm/starved/ring-high=2282730/0/5465/6295680/210/108`.
The run received 2,282,730 bytes over approximately 251 seconds, yet the 4 MiB ring reached only
108 bytes. The guest was not blocked while preloading an entire song. Instead, one privileged
`OUT` exception per byte delivered only about 9 KiB/s, slower than MP3 consumption, causing both
decoder starvation and guest-thread occupancy.

**Confirmed:** The original `pumpito` loop around image offset `0x212FD` increments its source
cursor and current-frame byte count before `OUT DX,AL`, then increments `ECX` and compares the
count with the current MPEG frame length. Its original addresses are `0x00343420` for the source
cursor, `0x00343424` for available end, `0x00343418` for frame length, `0x0034341C` for frame
count, and `0x00343438` for the source buffer.

**Implemented:** The compressed ring now has a 4 KiB physical capacity and a logical `0xE00`-byte
`DEMAND` limit. Only an exact match of the `pumpito` loop signature, relocation, memory ranges,
and frame state permits one SPSC span write for the current frame tail. The HLE updates the source
cursor, frame count, and `ECX` exactly like the skipped byte loop. A mismatch or insufficient
space falls back to the byte path, and the signature is not enabled for other targets. MAME's
`0xE00` value was used only as hardware-contract reference; no MAME code was incorporated.

**Confirmed:** The automated probe verifies demand deassertion at `0xE00`, reassertion after a
512-byte consume, a 3584-byte high-water mark, a four-byte frame-tail plan, and `ECX=103` after a
three-byte partial commit. Win32 x86 Debug builds of `repiu` and `repiu_aot_probe` also pass.

**Unresolved:** Bounded 60-second and 90-second runs did not reach the main MP3 transfer because
of startup variation. Actual block-HLE activation, long listening, and concurrent rendering,
input, and scene transitions during music still require validation in the user's environment.

## 11. pumpito frame batch relocation 교정 (Task 458)

### 한국어

**확인됨:** 사용자 실행은 `arena byte fast path active`와 반복적인 `0x030212FD` EIP를
보였지만 최종 통계가 `batched=0`, `ring-high=58`이었습니다. 단계별 최초 거부 진단을 추가한
재현 실행은 `relocation` 실패와 실제/예상 주소
`0x04453420/0x04333420`, `0x04453438/0x04333438`을 기록했습니다.

**원인 확정:** `0x00343420` 등의 LE fixup target은 전체 image 상대 주소가 아니라 object 4
상대 offset입니다. Task 457 구현과 synthetic probe는 이를 image offset으로 해석하여 object 4
base 차이 `0x00120000`만큼 낮은 주소를 예상했습니다. 실행 EIP와 opcode signature는 정확했지만
relocated immediate 검증이 항상 실패했습니다.

**교정됨:** 실행 준비 시 selector binding의 target object 4 relocated base를 보존하고, frame
상태와 source buffer 주소는 이 base에 fixup target offset을 더해 계산합니다. probe도 실제
object 배치와 같은 `runtime base + 0x00110000` object base 및 target offset을 사용합니다.
span enqueue는 `0xE00` 논리 FIFO까지만 허용하여 512-byte 물리 headroom을 보존합니다.

**확인됨:** 45초 제한 실행은 `verified frame-tail batch active`를 출력했고 최종 통계는
`received/dropped/decoded/pcm/starved/batched/ring-high=123943/0/297/342144/6/123372/3638`
이었습니다. 이전 사용자 실행의 `batched=0`, starvation 45회와 달리 대부분의 압축 byte가
일괄 처리되었고 dropped byte가 없었습니다.

**미확정:** 숨김 제한 실행은 화면과 음악의 실제 동시 진행을 청감·시각 검증하지 않습니다.
사용자 환경에서 장시간 음악, 렌더링, 입력과 장면 전환을 함께 확인해야 합니다.

### English

**Confirmed:** The user's run showed `arena byte fast path active` and repeated EIP
`0x030212FD`, but ended with `batched=0` and `ring-high=58`. A reproduction with first-occurrence
diagnostics reported a `relocation` failure with actual/expected pairs
`0x04453420/0x04333420` and `0x04453438/0x04333438`.

**Root cause confirmed:** LE fixup targets such as `0x00343420` are offsets relative to object 4,
not addresses relative to the whole image. Task 457 and its synthetic probe treated them as image
offsets and expected addresses lower by the `0x00120000` object-base difference. EIP and the opcode
signature were correct, but relocated-immediate validation therefore failed every time.

**Corrected:** Execution setup now retains the relocated base for selector binding target object 4,
and frame-state and source-buffer addresses add their fixup target offsets to that base. The probe
uses the real layout with an object base at `runtime base + 0x00110000` and object-relative target
offsets. Span enqueue also stops at the logical `0xE00` FIFO level to preserve 512 bytes of physical
race headroom.

**Confirmed:** A bounded 45-second run printed `verified frame-tail batch active` and ended with
`received/dropped/decoded/pcm/starved/batched/ring-high=123943/0/297/342144/6/123372/3638`.
Unlike the user's previous `batched=0` run with 45 starvation events, most compressed bytes used
the batch path and no bytes were dropped.

**Unresolved:** A hidden bounded run cannot audibly or visually verify concurrent music and
rendering. Long music playback, rendering, input, and scene transitions still require validation
in the user's environment.

## 12. pumpito MP3 batch 제어 경계 (Task 459)

### 한국어

**확인됨:** 손상된 사용자 실행은 321,140 byte를 받았지만 MPEG frame을 138개만 decode하여
약 2,327 byte/frame을 보였습니다. `REPIU_PIU10_MP3_BATCH_AUDIT=1` 감사 실행에서 일괄
enqueue와 guest commit을 끄고 원본 byte loop의 실제 출력을 예측 tail과 비교했습니다.
byte, source cursor와 frame byte count는 일치했지만 source cursor `0x76C` 이후 첫 불일치는
예상/실제 `ECX=1900/0`이었고, 이후 약 100 byte마다 `100/0`이 반복되었습니다.

**원인 확정:** 원본 loop는 cursor가 `0x76C` 이상이고 `ECX`가 100 이상이면 보조 처리를
호출하고 현재 전송 함수를 반환합니다. 다음 호출에서 `ECX`는 0으로 재설정됩니다. 기존
frame-tail batch는 frame 끝까지 cursor와 `ECX`를 commit하여 이 관측 가능한 제어 경계를
건너뛰었고, 그 결과 압축 stream의 대부분이 parser에서 폐기되었습니다.

**교정 및 검증:** batch 길이는 cursor와 `ECX` 조건이 처음 함께 성립하는 지점까지만
허용됩니다. 교정 후 실제 감사 실행은 1,700개 계획 구간을 연속 통과했고 mismatch가
없었습니다. 일반 실행은 `verified frame-tail batch active`, 1,671 byte 뒤 playback 시작,
그리고 64 KiB 체크포인트에서 `received/decoded/batched=66410/127/65567`을 기록했습니다.
진행 중 FIFO·decoder 대기분을 포함한 값이지만 손상 실행의 2,327 byte/frame에서 크게
회복되었으며, 감사가 압축 byte 순서를 직접 입증합니다.

**미확정:** 숨김 실행으로 실제 첫 음악의 음질과 화면·입력 동시 진행은 판단할 수 없습니다.
최종 청감 및 gameplay 검증은 사용자 환경에서 필요합니다.

### English

**Confirmed:** The damaged user run received 321,140 bytes but decoded only 138 MPEG frames,
about 2,327 bytes per frame. With `REPIU_PIU10_MP3_BATCH_AUDIT=1`, the live audit disabled batch
enqueue and guest commit and compared the original byte loop against the predicted tail. Bytes,
source cursor, and frame byte count matched, but after source cursor `0x76C` the first mismatch was
predicted/actual `ECX=1900/0`; subsequent `100/0` differences recurred roughly every 100 bytes.

**Root cause confirmed:** Once cursor is at least `0x76C` and `ECX` is at least 100, the original
loop invokes auxiliary processing and returns from the transfer function. The next call resets
`ECX` to zero. The old full-frame batch committed cursor and `ECX` through the frame end, skipping
this observable control boundary and causing the parser to discard most compressed input.

**Corrected and verified:** Batch length is now limited through the first point where the cursor
and `ECX` conditions become true. The corrected live audit passed 1,700 consecutive planned
segments without a mismatch. A normal run reported `verified frame-tail batch active`, playback
after 1,671 bytes, and `received/decoded/batched=66410/127/65567` at the 64 KiB checkpoint. The
in-progress snapshot includes FIFO and decoder backlog, but it is a large recovery from the damaged
2,327-byte/frame run, while the audit directly proves compressed-byte ordering.

**Unresolved:** A hidden run cannot judge first-track sound quality or concurrent rendering and
input. Final listening and gameplay validation remain for the user's environment.

## 13. pumpito MP3 decoder 입력 분할 의존성 (Task 460)

### 한국어

**확인됨:** 최신 사용자 실행은 `01.AUD` 전체 255,625 byte를 drop 없이 받았지만 MPEG
frame을 8개만 decode했고, 첫 64 KiB 체크포인트에서는 4개만 decode했습니다. 정상 SDL
종료 요청이 기록되었으므로 프로세스 crash가 아니라 음악 손상 뒤 사용자가 종료한
실행입니다.

`REPIU_PIU10_MP3_STREAM_AUDIT=1`로 producer와 consumer가 실제 처리한 byte를 각각
고정 4 KiB FNV-1a 구간으로 기록했습니다. 일반 batch 실행에서 완성된 62개 구간의 hash가
모두 일치하여 SPSC ring의 누락·중복·순서 변경을 배제했습니다. 더 결정적으로, 동일한
62개 producer hash를 가진 두 실행이 첫 64 KiB에서 각각 4 frame과 127 frame을 decode하여
입력 내용이 아닌 worker pop 시점이 결과를 바꾸고 있음을 확인했습니다.

**원인 확정:** 공용 MPEG parser는 현재 frame 길이를 이미 알고 있지만 worker는 해당
frame 뒤에 FIFO에서 꺼낸 모든 byte를 `minimp3`에 전달했습니다. buffer 끝에 다음 header의
일부만 포함되면 minimp3가 현재 sync를 승인하지 않고 `frame_bytes=0`을 반환할 수 있습니다.
worker는 이를 손상으로 오인하여 한 byte를 버리고 decoder를 reset했고, 동일 stream도 pop
분할에 따라 대량의 frame을 잃었습니다.

**교정 및 검증:** 연속 header로 검증된 정확히 한 non-free-format MPEG frame만 minimp3에
전달합니다. 또한 MAME의 MAS3507D 계약을 참고하여 decode 직후가 아니라 SDL queue 소비가
각 PCM frame 시작 offset에 도달할 때 PIU10 frame-sync 상태를 전이합니다. 독립적인 실제
실행 두 번은 첫 64 KiB에서 각각 `66408/127/65566`, `66409/127/65567`의
received/decoded/batched 값을 기록했습니다. 두 실행 모두 producer/consumer 62개 구간이
전부 일치했고 두 번째 실행의 불일치는 0개였습니다. 공용 probe도 호출 분할과 무관한
구간 hash를 확인합니다.

**미확정:** 자동화된 숨김 실행은 실제 첫 음악의 음질이나 화면·입력의 동시 진행을
청감·시각적으로 판단하지 못합니다. 사용자 환경에서 최종 검증이 필요합니다.

### English

**Confirmed:** The latest user run received all 255,625 bytes of `01.AUD` without drops but
decoded only eight MPEG frames, including only four by the first 64 KiB checkpoint. A normal SDL
exit request was logged, so this was a user exit after broken music rather than a process crash.

With `REPIU_PIU10_MP3_STREAM_AUDIT=1`, producer and consumer independently recorded bytes they
actually processed as fixed 4 KiB FNV-1a chunks. All 62 complete hashes matched in a normal batch
run, excluding omission, duplication, or reordering in the SPSC ring. More decisively, two runs
with identical hashes for all 62 producer chunks decoded four versus 127 frames at the first
64 KiB. Worker pop timing, not input content, was changing the result.

**Root cause confirmed:** The shared MPEG parser already knew the current frame length, but the
worker passed minimp3 that frame plus every later byte available from the FIFO. When the buffer
ended with only part of the next header, minimp3 could reject current sync and return
`frame_bytes=0`. The worker misclassified this as corruption, discarded one byte, reset the
decoder, and consequently lost many frames depending on FIFO-pop segmentation.

**Corrected and verified:** minimp3 now receives exactly one non-free-format MPEG frame already
validated through consecutive headers. Following the MAS3507D contract observed in MAME, PIU10
frame-sync also changes when SDL queue consumption reaches each PCM frame start rather than
immediately after decode. Two independent live runs recorded `66408/127/65566` and
`66409/127/65567` for received/decoded/batched at the first 64 KiB. All 62 producer/consumer chunks
matched in both runs, with zero mismatches in the second. The shared probe also verifies that chunk
hashes are independent of call segmentation.

**Unresolved:** Automated hidden runs cannot judge first-track sound quality or visually confirm
concurrent rendering and input. Final validation remains in the user's environment.

## 14. pumpito DAC3350A 곡 경계와 MP3 지연 (Task 461)

### 한국어

**확인됨:** PIU10 목적지 `0x010`의 data bit 1/0은 각각 DAC3350A SDA/SCL입니다. 실제 `pumpito` 감사 실행에서는 MP3 시작 전에 `AVOL=0x0101`이 여러 번 기록되고 재생 구간에서 `0x2C2C`가 기록됩니다.

**Task 462에서 정정됨:** `0x2C2C → 0x0101`은 곡 종료 경계가 아닙니다. gameplay 로그에서 한 곡 도중 이 전이가 세 번 발생했고, 연결했던 stop generation 1과 2는 각각 약 25초와 30초에 decoder와 FIFO를 지워 음악을 끊었습니다. 두 번째 stop은 같은 압축 입력에서 재동기화한 지 약 1초 뒤였습니다. 최종 통계는 1,703,110 byte 수신, drop 0이므로 입력 손실이 아니라 잘못된 stop 정책이 직접 원인입니다.

공식 MAME 참고 구현에 따르면 AVOL 0은 mute이고, 1~7은 -75~-57 dB의 3 dB 계단, 8 이상은 -54 dB부터 1.5 dB 계단이며 +18 dB에서 제한됩니다. 따라서 `0x01`은 -75 dB, `0x2C`(44)는 0 dB입니다. 현재 HLE는 이 값을 linear gain으로 바꾸어 SDL stream 출력에만 적용하고 decoder, 압축 FIFO와 queued PCM을 폐기하지 않습니다.

**설계값:** 사용자 관측에 따라 `pumpito` 시작 지연 기본값은 50 ms입니다. 44.1 kHz stereo S16에서는 8,820 byte의 무음이며 `REPIU_PIU10_MP3_LATENCY_MS=0..500`으로 조절할 수 있습니다.

**미확정:** 50 ms의 최종 체감 sync, 저음량 전이 뒤 곡 연속성과 실제 출력 음량은 사용자의 화면·음향 환경에서 확인해야 합니다. pause는 요구 범위에서 제외되었습니다.

**2026-08-10 교정:** 후속 실기 검증에 따라 `pumpito` profile의 50 ms 기본 지연을 제거했습니다.
모든 PIU10 profile은 이제 0 ms가 기본이며, 지연이 필요할 때만
`REPIU_PIU10_MP3_LATENCY_MS=0..500`으로 명시합니다. 50 ms를 8,820 byte 무음으로 변환하는
기능은 환경변수 override 검증을 위해 유지합니다.

### English

**Confirmed:** Data bits 1/0 at PIU10 destination `0x010` are DAC3350A SDA/SCL. A live `pumpito` audit recorded repeated `AVOL=0x0101` before MP3 startup and `0x2C2C` during playback.

**Corrected by Task 462:** `0x2C2C → 0x0101` is not a song-end boundary. A gameplay log showed the transition three times within a song. The attached stop generations 1 and 2 cleared the decoder and FIFO at roughly 25 and 30 seconds; the second occurred about one second after reacquiring the same compressed input. Final statistics recorded 1,703,110 input bytes and zero drops, making the incorrect stop policy—not input loss—the direct cause.

The official MAME reference maps AVOL zero to mute, values 1–7 to -75 through -57 dB in 3 dB steps, and values from 8 upward in 1.5 dB steps starting at -54 dB and clamped at +18 dB. Thus `0x01` is -75 dB and `0x2C` (44) is 0 dB. The current HLE converts this to linear gain and changes only SDL stream output, without discarding decoder state, compressed FIFO, or queued PCM.

**Design value:** Based on the user's observation, `pumpito` defaults to 50 ms startup latency. This is 8,820 silence bytes for 44.1 kHz stereo S16 and can be adjusted with `REPIU_PIU10_MP3_LATENCY_MS=0..500`.

**Unresolved:** Final subjective 50 ms alignment, song continuity through a low-volume transition, and actual output level require validation with the user's real display/audio path. Pause is outside scope.

**2026-08-10 correction:** Following later live validation, the 50 ms default was removed from the
`pumpito` profile. Every PIU10 profile now defaults to 0 ms; a delay is applied only when explicitly
requested with `REPIU_PIU10_MP3_LATENCY_MS=0..500`. Conversion of 50 ms to 8,820 silence bytes
remains as coverage for the environment override.

## 15. DAC 저음량 시점의 audio backlog 계측 (Task 463)

### 한국어

**확인됨:** Task 462까지의 DAC 감사는 `AVOL=0x0101`이 출력 gain을 `0.000177`로 내린
사실만 보여 주며, 그 순간 HLE에 남은 재생 데이터는 기록하지 않았습니다. 따라서 guest가
자신의 곡 시각에 맞춰 DAC를 내렸지만 HLE 출력이 뒤처졌다는 가설은 기존 로그만으로
확정할 수 없습니다.

**구현됨:** `Piu10Mp3AudioOut::Snapshot()`은 DAC transaction 시점의 SDL 입력 PCM queue,
현재 S16 형식 기준 대기 밀리초, audio device callback buffer, compressed SPSC ring,
worker 내부 decoder pending byte, receive/decode 계수와 frame-sync를 반환합니다.
`REPIU_PIU10_DAC_AUDIT=1`은 이를 DAC transaction과 같은 줄에 기록합니다. SDL 공식 계약상
`SDL_GetAudioStreamQueued()`는 stream에 넣은 미소비 입력 byte 수이며 모든 thread에서
호출할 수 있습니다. device buffer frame 수 역시 별도로 조회합니다.

**해석 제한:** device buffer는 현재 남은 정확한 양이 아니라 SDL이 hardware에 공급하는
한 chunk의 크기입니다. 또한 서로 다른 입력 형식의 PCM이 동시에 queue에 있으면 SDL은
통합 byte 수만 반환하므로 `pcm-queued-ms`는 현재 형식 기준 근사치입니다. raw byte,
sample rate와 channel 수를 함께 보존합니다.

**Task 464에서 확인됨:** 실제 음악 중단의 `0x0101`은 PCM 261.995 ms와 compressed
backlog 291,066 byte를 남겼습니다. 이는 약 18.2초의 압축 재생분이므로 guest DAC 시각이
HLE 출력보다 크게 앞섰다는 가설이 확정되었습니다.

참고: [SDL_GetAudioStreamQueued](https://wiki.libsdl.org/SDL3/SDL_GetAudioStreamQueued),
[SDL_GetAudioDeviceFormat](https://wiki.libsdl.org/SDL3/SDL_GetAudioDeviceFormat).

### English

**Confirmed:** Through Task 462, DAC audit showed only that `AVOL=0x0101` reduced output gain to
`0.000177`; it did not record playable HLE backlog at that instant. The existing log therefore
cannot prove the hypothesis that the guest lowered the DAC on its own song timeline while HLE
output lagged behind.

**Implemented:** `Piu10Mp3AudioOut::Snapshot()` returns the SDL input PCM queue and duration in the
current S16 format, audio-device callback buffer, compressed SPSC ring, decoder-pending worker
bytes, receive/decode counters, and frame-sync at a DAC transaction. `REPIU_PIU10_DAC_AUDIT=1`
appends these values to the same transaction record. Under SDL's official contract,
`SDL_GetAudioStreamQueued()` returns unconsumed input bytes put into the stream and is safe from
any thread. Device-buffer frames are queried separately.

**Interpretation limit:** Device-buffer size is one SDL hardware-feed chunk, not exact current
occupancy. If differently formatted input PCM coexists in the queue, SDL returns one aggregate
byte count, making `pcm-queued-ms` an approximation in the current format. Raw bytes, sample rate,
and channel count remain in the record.

**Confirmed by Task 464:** The interrupting `0x0101` left 261.995 ms of PCM and 291,066 bytes of
compressed backlog. This represents about 18.2 seconds of compressed playback, confirming that
the guest DAC timeline substantially led HLE output.

References: [SDL_GetAudioStreamQueued](https://wiki.libsdl.org/SDL3/SDL_GetAudioStreamQueued),
[SDL_GetAudioDeviceFormat](https://wiki.libsdl.org/SDL3/SDL_GetAudioDeviceFormat).

## 16. decoder inflight 기반 MAS3507D backpressure (Task 464)

### 한국어

**원인 확정:** 중단 시점은 `compressed-ring=3311`, `decoder-pending=287755`로 총
291,066 byte를 남겼습니다. 약 128 kbps에서 약 18.2초입니다. 앞선 전이는 약 72 KiB가
173 frame, 약 4.52초 동안 drain되어 같은 환산을 검증합니다. 고정 512-byte ring pop 뒤
PCM queue가 허용한 약 417-byte frame 하나만 소비되면서 약 95 byte/frame이 staging에
누적되었고, ring 크기만 본 DEMAND가 이를 guest에 노출하지 않았습니다.

**구현됨:** platform-neutral `sound::DecoderInputFifo`는 guest 수락 시 inflight를 예약하고
ring에서 staging으로 이동할 때 유지하며, parser cursor가 실제 전진한 byte만 차감합니다.
batch와 DEMAND는 `0xE00` 논리 inflight를 사용합니다. byte path는 stale status race를 위해
4,096 byte까지 허용합니다. invalid prefix와 rejected frame은 소비되고 incomplete input은
소비되지 않습니다.

**검증됨:** synthetic probe는 `0xE00` fill에서 DEMAND low, 512-byte pop 뒤에도 low 유지,
417-byte consume 뒤 high 복귀, batch refill 제한과 4 KiB byte headroom을 확인했습니다.
결과는 `demand_low=true,pop_keeps_low=true,demand_high=true,inflight_high=4096`입니다.

**사용자 검증:** 2026-08-10 사용자 gameplay 재실행에서 기존의 곡 중도 끊김 증상이
사라졌습니다. 따라서 decoder inflight 기반 backpressure가 해당 증상에 유효한 대책임은
확인되었습니다. 중단 예상 지점의 실제 `compressed-inflight` 수치와 노트 sync의 정밀
측정은 아직 수행하지 않았습니다.

### English

**Root cause confirmed:** The interruption left `compressed-ring=3311` and
`decoder-pending=287755`, totaling 291,066 bytes or about 18.2 seconds at roughly 128 kbps. An
earlier transition drained about 72 KiB over 173 frames, about 4.52 seconds, independently
validating the conversion. A fixed 512-byte ring pop followed by consumption of the one roughly
417-byte frame allowed by the PCM queue accumulated about 95 bytes per frame in staging, while
ring-only DEMAND hid that backlog from the guest.

**Implemented:** Platform-neutral `sound::DecoderInputFifo` reserves inflight bytes at guest
acceptance, keeps them while moving from ring to staging, and subtracts only actual parser-cursor
progress. Batch input and DEMAND use the logical `0xE00` inflight level. The byte path permits up
to 4,096 bytes for stale-status race headroom. Invalid prefixes and rejected frames are consumed;
incomplete input is not.

**Verified:** The synthetic probe checks low DEMAND at `0xE00`, persistence after a 512-byte pop,
high DEMAND only after a 417-byte consume, batch-refill limiting, and 4 KiB byte headroom. It
reports `demand_low=true,pop_keeps_low=true,demand_high=true,inflight_high=4096`.

**User validation:** On 2026-08-10, the user reran the gameplay path and reported that the previous
mid-song interruption no longer occurred. Decoder-inflight backpressure is therefore confirmed as
an effective mitigation for that symptom. The actual `compressed-inflight` value at the former
interruption point and precise note synchronization have not yet been measured.

## 17. PIU10 MP3 batch의 바이너리 독립화 (Task 465)

### 한국어

**확인됨:** 기존 frame-tail batch는 `pumpito` ROM-set 이름, image offset `0x212FD`, LE object
4와 `0x3434xx` data offset을 직접 사용했습니다. MP3 decoder, FIFO와 단일-byte fast path는
PIU10 capability 공용이었지만 처리량을 결정하는 batch만 특정 바이너리 배치에 묶여
있었습니다.

**교정됨:** target 이름과 모든 고정 code/object/data offset을 제거했습니다. matcher는 현재
`OUT DX,AL`의 bounded 주변 코드에서 검증된 feeder shape를 확인하고, instruction operand와
backward/forward branch target에서 cursor, available end, source buffer, frame count/target과
주기적 service 경계를 추출합니다. 추출한 주소의 alias와 runtime 범위까지 일치해야 하며,
실패하면 guest 상태를 바꾸지 않고 scalar byte path를 유지합니다. batch와 stream audit는
모든 PIU10-capable target에서 사용할 수 있습니다.

**검증됨:** 기존 상수와 다른 synthetic code/data 위치에서 4-byte plan과 3-byte partial commit
후 `ECX=103`을 확인했습니다. cursor operand 하나를 다른 주소로 바꾸면 matcher가
fail-closed했습니다. probe 결과는
`piu10_mp3_frame_batch=true,bytes=4,ecx=103,relocated=true,fail-closed=true`입니다. Win32 x86
Debug `repiu`와 `repiu_aot_probe` 빌드도 성공했습니다.

**추가 확인:** 사용자 `pumpitc` 실행은 `verified frame-tail batch active`를 기록했고 최종
5,530,320 byte 중 5,452,054 byte(약 98.6%)를 batch 처리했습니다. 반면 수정 전 `pumpite`
실행은 `OUT DX,AL`의 relocated EIP `0x0402D167`(object 2 `+0x1D167`)에서 shape를 거부하고
83,376 byte를 모두 scalar로 처리했습니다. 정적 명령열은 같은 cursor/count/frame-target
계약과 같은 backward loop `+0x1D04B`, service 조건 `ECX < 100` 및 cursor `< 0x76C`를
사용하지만, frame count register가 `EBP`에서 `EDX`로, frame target register가 `EDX`에서
`EBX`로 바뀌고 독립 갱신 명령의 순서도 달랐습니다.

**추가 교정 및 검증:** prefix/suffix matcher는 제한된 x86 명령을 decode하여 cursor와
frame-count의 load/increment/store alias, register def-use와 `ESI -> EDX` port restore 전
clobber 순서를 검증합니다. 임시 register 번호와 독립 명령 순서는 고정하지 않습니다. 기존
schedule과 `pumpite` schedule을 서로 다른 synthetic 위치에 배치한 probe가 모두 같은 4-byte
plan을 만들었고, 기존 alias 손상 arm도 계속 fail-closed했습니다. 결과는
`piu10_mp3_frame_batch=true,bytes=4,ecx=103,relocated=true,variant=true,fail-closed=true`입니다.
Win32 x86 Debug `repiu_aot_probe`와 `repiu` 빌드가 성공했습니다. 수정 후 실제 `pumpite`
batch 활성화는 아래 사용자 실기 로그에서 확인했습니다.

**사용자 실기 검증:** 수정 후 `pumpite` 로그는 `verified frame-tail batch active`를 기록했고,
첫 checkpoint에서 66,478 byte 중 65,596 byte(약 98.7%)를 batch 처리했습니다. 이전 실행의
`batched=0`과 달리 313초 이상 guest 실행과 입력 처리가 계속됐으며, 사용자는 상태가
좋아졌다고 확인했습니다. 이 캡처에는 정상 종료 통계가 없으므로 전체 실행의 최종 batch
비율은 측정하지 않았습니다.

### English

**Confirmed:** The former frame-tail batch directly depended on the `pumpito` ROM-set name, image
offset `0x212FD`, LE object 4, and `0x3434xx` data offsets. The decoder, FIFO, and scalar-byte fast
path were PIU10-capability features, but the throughput-critical batch remained tied to one binary
layout.

**Corrected:** The target name and every fixed code, object, and data offset are removed. The
matcher validates a bounded feeder shape around the current `OUT DX,AL`, then derives the cursor,
available end, source buffer, frame count/target, and periodic-service boundary from instruction
operands and backward/forward branch targets. Operand aliases and runtime ranges must also agree;
otherwise no guest state changes and the scalar byte path remains active. Batch and stream audits
are available to every PIU10-capable target.

**Verified:** A synthetic loop at code and data locations unrelated to the former constants
produced a four-byte plan and `ECX=103` after a three-byte partial commit. Redirecting one cursor
operand made the matcher fail closed. The probe reports
`piu10_mp3_frame_batch=true,bytes=4,ecx=103,relocated=true,fail-closed=true`. Win32 x86 Debug builds
of `repiu` and `repiu_aot_probe` also succeeded.

**Additional confirmation:** A user `pumpitc` run logged `verified frame-tail batch active` and
batched 5,452,054 of 5,530,320 bytes, about 98.6%. Before this correction, a `pumpite` run rejected
the shape at relocated `OUT DX,AL` EIP `0x0402D167` (object 2 `+0x1D167`) and processed all 83,376
bytes through the scalar path. Static instructions use the same cursor/count/frame-target contract,
the same backward loop at `+0x1D04B`, and the same service conditions `ECX < 100` and cursor
`< 0x76C`; however, the frame-count register changes from `EBP` to `EDX`, the target register from
`EDX` to `EBX`, and independent updates are reordered.

**Additional correction and verification:** The prefix/suffix matcher now decodes a restricted x86
instruction set and validates cursor and frame-count load/increment/store aliases, register def-use,
and clobber ordering before the `ESI -> EDX` port restore. Temporary-register numbers and ordering
of independent instructions are not fixed. Synthetic loops for both the original and `pumpite`
schedules produce the same four-byte plan, while the existing corrupted-alias arm still fails
closed. The probe reports
`piu10_mp3_frame_batch=true,bytes=4,ecx=103,relocated=true,variant=true,fail-closed=true`.
Win32 x86 Debug builds of `repiu_aot_probe` and `repiu` succeeded. The following live user log
confirms post-fix `pumpite` activation.

**Live user validation:** After the correction, the `pumpite` log records
`verified frame-tail batch active` and batches 65,596 of 66,478 bytes at the first checkpoint,
about 98.7%. Unlike the former `batched=0` run, guest execution and input processing continue for
more than 313 seconds, and the user confirms that behavior improved. The capture has no orderly
shutdown statistics, so the final batch ratio over the full run was not measured.

## 18. 호출 래퍼형 PIU10 MP3 feeder (Task 471)

### 한국어

**확인됨:** `pumpitpc`는 runtime `0x030EC755`의 `OUT DX,AL`을 직접 feeder 안에 두지 않고,
`push ebx; mov ebx,eax; mov al,dl; mov edx,ebx; out dx,al; pop ebx; ret` 래퍼를
`0x03019461`에서 호출합니다. feeder는 호출 전에 source cursor와 frame count 및 `ECX`를
증가시키고, 반환 후 count/target을 비교해 `0x03019380`으로 되돌아갑니다. 기존 direct
matcher는 이 구조를 거부하여 이전 로그의 542,517 byte가 모두 scalar였고 privileged
instruction exception이 393만 회에 달했습니다.

**교정됨:** 고정 주소나 target 이름 없이 래퍼 명령열, guest stack의 반환 주소, 상대 call
대상, 호출 전 상태 alias, 반환 후 backward edge와 service 조건을 함께 검증하는 matcher를
추가했습니다. synthetic probe는 relocated direct 두 변형과 wrapped 변형을 모두 승인하고,
손상된 alias와 wrapped 반환 주소를 거부합니다. 결과는
`piu10_mp3_frame_batch=true,bytes=4,ecx=103,relocated=true,variant=true,wrapped=true,fail-closed=true`
입니다.

**감사 교정:** 최초 wrapped 감사에서 byte/cursor/count는 일치했지만, FIFO inflight가
`0xE00`에 도달해 scalar status polling이 반환한 지점을 예측 구간이 넘으면서 `ECX`만
불일치했습니다. enqueue하지 않는 감사 계획도 현재 inflight의 `DEMAND` 여유에서 끊도록
교정한 뒤 실제 Release 실행에서 3,700개 연속 segment가 mismatch 없이 통과했습니다.

**실행 검증:** Win32 x86 Debug와 Release 빌드 및 전체 AOT probe가 성공했습니다. 25초
Release 실행은 `verified frame-tail batch active`, playback 시작 2,902 byte와
`received/dropped/decoded/pcm/starved/batched/ring-high/inflight/inflight-high=`
`140795/0/329/379008/0/136595/2368/2212/3584`를 기록했습니다. 수신 byte의 약 97.0%가
batch 처리됐고 drop과 starvation은 없었습니다.

### English

**Confirmed:** `pumpitpc` does not place the `OUT DX,AL` at runtime `0x030EC755` directly in the
feeder. A call at `0x03019461` enters a `push ebx; mov ebx,eax; mov al,dl; mov edx,ebx; out dx,al;
pop ebx; ret` wrapper. Before the call, the feeder advances the source cursor, frame count, and
`ECX`; after return, it compares count/target and branches back to `0x03019380`. The former direct
matcher rejected this shape, leaving all 542,517 bytes in the earlier log on the scalar path and
raising 3.93 million privileged-instruction exceptions.

**Corrected:** A matcher now validates the wrapper instructions, guest-stack return address,
relative call target, pre-call state aliases, post-return backward edge, and service conditions,
without a fixed address or target name. The synthetic probe accepts two relocated direct variants
and the wrapped variant while rejecting a corrupted alias and wrapped return address. It reports
`piu10_mp3_frame_batch=true,bytes=4,ecx=103,relocated=true,variant=true,wrapped=true,fail-closed=true`.

**Audit correction:** The first wrapped audit matched byte/cursor/count but predicted past the
point where scalar status polling returned when FIFO inflight reached `0xE00`, leaving only `ECX`
mismatched. Limiting the non-enqueuing audit plan to current `DEMAND` headroom produced 3,700
consecutive matching segments in a live Release run.

**Runtime verification:** Win32 x86 Debug and Release builds and the complete AOT probe succeeded.
A 25-second Release run logged `verified frame-tail batch active`, playback after 2,902 bytes, and
`received/dropped/decoded/pcm/starved/batched/ring-high/inflight/inflight-high=`
`140795/0/329/379008/0/136595/2368/2212/3584`. About 97.0% of received bytes were batched, with no
drops or starvation.

## 19. PIU10 후속 타이틀 프로파일 (Task 472)

### 한국어

**외부 사양으로 확인됨:** MAME 공식
[`xtom3d.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d.cpp)는
`pumpitpr`, `pumpitpx`, `pumpit8`, `pumpitp2`, `pumpipx2`, `pumpitp3`, `pumpipx3`을 모두
`PUMPITUP_BIOS` 아래에 두며, 각 세트에 8-byte CAT702 region과 CD image를 정의합니다.
따라서 이들은 기존 `pumpite`와 같은 PIU10/CAT702/JAMMA capability 계약으로 등록했습니다.

**로컬 자산 확인:** 일곱 short name의 ZIP은 모두 존재하며 공용 mount 준비 단계의
`piu10.u8`, `piu10.u9`, `<short-name>.cat702` 검증을 통과했습니다. 현재 대응 CHD 디렉터리는
없으므로 analyzer는 각 id에 대해 `<id> CHD directory not found`로 종료했습니다. 이는 profile
lookup과 ROM-set 선택이 성공했음을 확인하지만, ISO mount, 실행 파일 구조와 gameplay는 아직
확인하지 못한 상태입니다.

**구현 검증:** profile probe는 PIU10 11개 profile 각각의 단일 등록, 공식 경로,
`rom_set_id`, `piu_common`, JAMMA/PIU10/CAT702 true와 latency 0 ms를 확인합니다. Debug와
Release 빌드 및 두 구성의 probe가 통과했습니다.

### English

**Confirmed from the external specification:** MAME's official
[`xtom3d.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d.cpp) places
`pumpitpr`, `pumpitpx`, `pumpit8`, `pumpitp2`, `pumpipx2`, `pumpitp3`, and `pumpipx3` under
`PUMPITUP_BIOS`, with an eight-byte CAT702 region and CD image for every set. They are therefore
registered with the same PIU10/CAT702/JAMMA capability contract as the existing `pumpite`.

**Local asset confirmation:** ZIPs for all seven short names exist and pass the shared mount
preparation checks for `piu10.u8`, `piu10.u9`, and `<short-name>.cat702`. Their CHD directories are
currently absent, so the analyzer exits with `<id> CHD directory not found` for each one. This
confirms profile lookup and ROM-set selection, but ISO mounting, executable structure, and gameplay
remain unverified.

**Implementation verification:** The profile probe validates the single registration, canonical
paths, `rom_set_id`, `piu_common`, enabled JAMMA/PIU10/CAT702 capabilities, and zero latency for
each of the eleven PIU10 profiles. Debug and Release builds and probes pass.

## 20. MAME PIU target profile 전체 카탈로그 (Task 485)

### 한국어

**외부 사양으로 확인됨:** MAME 공식
[`xtom3d.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d.cpp)의
`GAME` 선언은 `pumpit1`부터 `pumpipx3b`까지 22개 PIU 세트를 일정한 순서로 나열합니다.
기존 registry에는 14개만 있어 `pumpit2a`, `pumpit3a`, `pumpitpru`, `pumpitea`,
`pumpipx2p`, `pumpitp3a`, `pumpipx3a`, `pumpipx3b`가 누락돼 있었습니다.

**구현됨:** 22개 profile을 MAME 순서로 등록하고 display name을 상세 버전·연도·날짜가
포함된 이름으로 교체했습니다. 브랜드 대소문자는 모두 `Pump It Up`으로 통일했습니다.
반복되는 경로, reservation과 capability aggregate는 공용 factory가 생성합니다. 신규 clone은
동일 세대 parent와 같은 JAMMA/PIU10/CAT702 capability를 사용합니다.

**정리됨:** 수동 `MASTER/PIU_1ST` 경로를 사용하던 `piu_1st` profile을 제거했습니다.
실행기, supervisor와 analyzer의 무인자 기본 target은 `pumpit1`이며 setup/test asset 검사도
`roms/pumpit1.zip`과 `roms/pumpit1/*.chd`를 기준으로 합니다.

**검증됨:** Debug/Release 네 실행 파일 빌드와 두 구성의 전체 coherence probe가
통과했습니다. registry probe는 `piu10_target_profiles=true`,
`jamma_target_profiles=true`를 반환했고, 무인자 Debug analyzer는 `Target: pumpit1`과
상세 display name을 출력했습니다.

### English

**Confirmed from the external specification:** The `GAME` declarations in MAME's official
[`xtom3d.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/misc/xtom3d.cpp) list
22 PIU sets in a stable order from `pumpit1` through `pumpipx3b`. The previous 14-entry
registry omitted `pumpit2a`, `pumpit3a`, `pumpitpru`, `pumpitea`, `pumpipx2p`,
`pumpitp3a`, `pumpipx3a`, and `pumpipx3b`.

**Implemented:** All 22 profiles now follow MAME order and use detailed display names with
version, year, or date information. Brand capitalization is normalized to `Pump It Up`.
A shared factory builds repeated paths, reservations, and capability aggregates. Each new
clone uses the JAMMA/PIU10/CAT702 capabilities of its parent generation.

**Cleaned up:** The manual `piu_1st` profile for `MASTER/PIU_1ST` was removed. The runner,
supervisor, and analyzer now default to `pumpit1`, and setup/test asset checks use
`roms/pumpit1.zip` plus `roms/pumpit1/*.chd`.

**Verified:** All four executables built in Debug and Release, and the full coherence probe
passed in both configurations. The registry probe reported `piu10_target_profiles=true` and
`jamma_target_profiles=true`; the no-argument Debug analyzer selected `Target: pumpit1` and
printed its detailed display name.

## 21. PIU10 MP3 재생 시계 (Task 740)

**확인됨:** frame-sync(status bit 2)와 DEMAND(bit 0)는 Task 460·464 이후 SDL 장치가 가져간 byte 수에
매여 있었고, 그 수는 장치 buffer 단위로 움직입니다. WSLg(768 frame, 17 ms)에서는 frame당 토글 하나가
유지되지만, buffer를 2048 frame으로 강제하면 초당 4~6회, 4096 frame이면 토글의 40%가 한 worker
호출에 두 개 이상 겹쳤습니다. decoder도 같은 계단으로 다시 채워 DEMAND와 guest의 frame 카운터가
계단으로 움직였습니다. 이제 `Piu10Mp3AudioOut`은 직전 계단의 byte 수에서 PCM 속도로 올라가고 가져간
수를 넘지 않는 재생 시계를 두고, frame-sync 전이와 decode 게이트(시계 앞 0.25초, 큐가 그 절반
아래면 즉시)를 그 시계로 판단합니다. 최종 보고 한 줄(`PIU10 MP3 received/…/device-buffer-frames`)과
`REPIU_PIU10_MP3_CENSUS_MS=<ms>` census가 토글·다중 토글·기아·빈 큐·시계 지연을 냅니다.

**미확정:** 사용자 host의 노이즈는 WSLg에서 재현되지 않았습니다. 곡 중 PCM 큐는 235 ms 아래로
내려가지 않았습니다.

### English

**Confirmed:** Since Tasks 460 and 464, frame-sync (status bit 2) and DEMAND (bit 0) were tied to the
byte count the SDL device had pulled, which moves in device-buffer steps. On WSLg (768 frames, 17 ms)
one toggle per frame held, but forcing the buffer to 2048 frames gave four to six collapsed toggles a
second and 4096 frames collapsed 40% of them into one worker call. The decoder refilled in the same
steps, so DEMAND and the guest's frame counter moved in steps too. `Piu10Mp3AudioOut` now keeps a
playback clock that rises at the PCM rate from the count before the latest step and never passes the
pulled count; frame-sync transitions and the decode gate (a quarter second ahead of the clock, at once
if the queue drops below half of that) follow it. The final report line (`PIU10 MP3
received/…/device-buffer-frames`) and the `REPIU_PIU10_MP3_CENSUS_MS=<ms>` census give toggles,
multi-toggles, starvation, empty queue and clock lag.

**Unresolved:** The noise on the user's host did not reproduce on WSLg; the PCM queue never fell below
235 ms mid-song.

## 22. PIU10 곡 시계와 MP3 위치 덮어쓰기 (issue #58)

**확인됨 (pumpipx3 PIU.EXE, object 2 런타임 base 0x04010000):**

* 타이머 ISR 본체 `0x0401213D`는 tick마다 `[0x92aaf8]`(곡 시계)를 1 올리고 MP3 공급 함수 `0x0402D86B`를 부른다.
* 공급 함수는 0x2DA status를 읽어 bit 2(frame-sync)가 직전 값(`[0x9acf44]`)과 다르면 MP3 프레임 수
  `[0x9acf2c]`를 1 올리고, **프레임 수가 256의 배수가 되면 곡 시계를 `round(프레임 수 × 6.2688)`로 덮어쓴다**
  (setter `0x040121CD`, 상수는 object 4 + `0x180D`의 float). 이어 bit 0(demand)이면 bit 1(send-ready)을
  기다리며 바이트를 보낸다.
* 화살표는 getter `0x040121DD`로 곡 시계를 읽는다. 6.2688은 240 Hz × 1152/44100(= 6.2694)에 가까운 값이다.
* 0x2DA 접근은 공용 `inpw` `0x040FC0A1`/`outpw` `0x040FC0AA`를 거친다.

따라서 화살표는 평소 tick으로 진행하고 약 6.7초마다 MP3 위치로 끌려간다. 게임은 4 ms마다 도는 ISR이 26 ms
간격의 frame-sync 변화를 하나도 놓치지 않는다고 가정한다. **tick이 26 ms 넘게 끊기면 그 사이 토글 두 개가
상쇄되어 게임의 프레임 수가 2프레임(52 ms) 영구히 뒤처지고**, 다음 덮어쓰기에서 화살표가 그만큼 뒤로 가며 그
곡이 끝날 때까지 음악보다 늦다.

**측정 (Win32 Release, vsync, 60 Hz, 자동 플레이로 스테이지 821번 곡 두 번, 진단 `Piu10SongClockCensus`):**

| 실행 | 스테이지 덮어쓰기 점프 | 토글 손실 | 긴 present(>25 ms) |
|---|---|---|---|
| 창 캡처(PrintWindow 5초 간격) 동시 | −194.6 ~ +5.4 ms, 8 ms 이상 8회 | 16건(스테이지 안 6건) | 24 |
| 캡처 없음 | −15.4 ~ +5.4 ms | 4건, 모두 스테이지 밖 곡 전환 로딩 | 7 |

* 캡처 없는 스테이지의 ±5~15 ms는 Win32에서 vsync 대기 중 tick이 멈춰(스왑 대기 주입은 direct 모델에서
  유보, #6) 게스트가 토글을 0–16 ms 늦게 보는 양자화다. 프레임당 tick은 87%가 4, 0–2는 0.5%, 6 이상은 0.2%.
* 토글 손실의 두 경로: present가 vsync 여러 주기 막힘(캡처 등 외부 요인), 곡 전환 로딩에서 게스트 390–480 ms.

**미확정:** 사용자 환경(Linux 실기, 스팀덱, 고주사율 모니터, 녹화 도구)에서 어느 경로가 나오는지. 60 Hz가
아닌 화면에서는 vsync로 게임이 그 주사율로 돌아 프레임당 tick이 정수가 아니게 된다(144 Hz면 1.67).

**수정 (issue #58, `REPIU_PIU10_FRAME_SYNC_REPLAY`, 기본 켜짐):** 오디오 워커가 frame-sync를 뒤집은 시각을 기록하고
(`Piu10FrameSyncHistory`), 게스트의 0x2DA 상태 읽기는 핸들러 안이면 그 tick의 due 시각, 밖이면 마지막으로 전달된
tick의 due 시각의 비트로 답한다(`Piu10FrameSyncAnswerClock`, 되돌아가지 않음). 핸들러 밖 읽기를 "지금"으로 답한
첫 판은 CAT702 통신이 같은 포트를 읽어(상태 읽기의 70%) 답변 시각을 앞당겼고, +51 ms 가짜 변화와 −190~−449 ms
되돌아감이 남았다. 캡처를 켠 같은 조건에서 수정 뒤 스테이지 두 곡의 덮어쓰기 16회가 모두 +1.1 또는 −2.9 ms(tick
하나 안)였다. 남은 큰 점프는 타이틀 곡의 +26.1 ms 한 번으로, 게스트가 290 ms 멈춰 backlog 상한(64 tick)을 넘긴
tick 폐기다. 곡 사이(공급 함수가 쉬는 구간)의 다중 변화는 곡 위치와 무관하다. 사용자 플레이(8곡)에서 한 번
+55.3 ms가 나온 것은 워커가 live 비트를 뒤집고 이력에 기록하는 사이의 race였고, 이력이 있으면 이력의 마지막 값으로
답하도록 고쳤다. 후속은 #59.

### English

**Confirmed (pumpipx3 PIU.EXE, object 2 at runtime base 0x04010000):** the timer ISR body `0x0401213D` adds one
to the song clock `[0x92aaf8]` on every tick and calls the MP3 feeder `0x0402D86B`. The feeder reads the 0x2DA
status; when bit 2 (frame-sync) differs from its previous value (`[0x9acf44]`) it adds one to the MP3 frame count
`[0x9acf2c]`, and **when that count reaches a multiple of 256 it overwrites the song clock with `round(frames ×
6.2688)`** (setter `0x040121CD`, the float at object 4 + `0x180D`); then, while bit 0 (demand) is set, it waits for
bit 1 (send-ready) and sends bytes. The arrows read the song clock through the getter `0x040121DD`. 6.2688 is close
to 240 Hz × 1152/44100 (6.2694). Port 0x2DA goes through the shared `inpw` `0x040FC0A1` / `outpw` `0x040FC0AA`.

The arrows therefore run on ticks and are pulled to the MP3 position about every 6.7 s. The game assumes its 4 ms
ISR never misses a frame-sync change 26 ms apart. **When ticks stop for more than 26 ms, two toggles cancel and the
game's frame count falls two frames (52 ms) behind for good**: the next overwrite moves the arrows back by that much
and they stay behind the music for the rest of the song.

**Measured (Win32 Release, vsync, 60 Hz, an automated play through stage song 821 twice, `Piu10SongClockCensus`):**
with a window capture (PrintWindow every 5 s) running, stage overwrites ranged −194.6 to +5.4 ms with eight of 8 ms
or more, 16 lost-toggle events (six inside the stages) and 24 presents over 25 ms; without it, −15.4 to +5.4 ms, four
lost-toggle events all in song changes outside the stages, seven long presents. The ±5–15 ms is the observation
quantization of Win32, whose vsync wait carries no ticks (swap-wait injection is held back on the direct model, #6);
87% of frames carry four ticks, 0.5% zero to two, 0.2% six or more. The two loss paths are a present blocked over
several vsync periods (an outside cause such as the capture) and song-change loads of 390–480 ms in the guest.

**Unresolved:** which path the user's setups hit (Linux hardware, the Steam Deck, high-refresh screens, recording
tools). On a screen other than 60 Hz the game runs at that rate under vsync, so a frame no longer carries a whole
number of ticks (1.67 at 144 Hz).

**Fix (issue #58, `REPIU_PIU10_FRAME_SYNC_REPLAY`, on by default):** the audio worker records when it flips frame-sync
(`Piu10FrameSyncHistory`), and a guest read of the 0x2DA status is answered with the bit at the due time of the tick
whose handler reads it, or outside every handler at the latest delivered tick's (`Piu10FrameSyncAnswerClock`, never
backwards). A first version answering reads outside handlers at the present was moved ahead by the CAT702 exchange,
which reads the same port (70% of the status reads), and left a +51 ms phantom change and steps back of −190 to −449
ms. With the capture running, after the fix all 16 overwrites in two stages were +1.1 or −2.9 ms (within one tick).
The one large jump left, +26.1 ms in the title song, is ticks dropped when the guest stalled 290 ms past the backlog's
64-tick bound. Multiple changes between songs, where the feeder idles, do not touch the song position. A +55.3 ms
in the user's play (eight songs) was a race between the worker flipping the live bit and recording the flip; with a
history the answer now comes from its newest flip. Follow-ups are #59.
