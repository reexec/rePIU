# 아키텍처

## 목적

이 문서는 현재 구현되는 코드의 설계와 구조를 지속적으로 기록한다.

코드 구조가 바뀌거나 새 하위 시스템이 추가되면, 해당 작업의 설계 문서와 함께 이 문서를 갱신해야 한다.

## Purpose

This document continuously records the design and structure of the code currently being implemented.

When code structure changes or a new subsystem is added, this document must be updated together with the task design document.

## 현재 상태

현재 저장소는 원본 DOS/4G 실행 파일을 네이티브 x86으로 실행하고 주변 DOS·DPMI·하드웨어
경계를 HLE로 제공하는 Win32 runtime과 분석 도구를 구현합니다.

## Current State

The repository implements a Win32 runtime and analysis tools that execute original
DOS/4G binaries as native x86 while providing DOS, DPMI, and hardware boundaries through HLE.

## 예정 구조

예정된 공용 구조:

* `include/`: 외부에서 참조 가능한 프로젝트 C++ 헤더
* `src/`: 플랫폼 공용 로더와 런타임 코어
* `src/engine/`: 플랫폼 공용 실행 엔진 — AOT/DBT, 경계 처리, 실행 트램폴린, 텔레메트리,
  명령 에뮬레이션, DOS/BIOS/포트 I/O HLE, Glide. **Task 520 이전에는 이 코드가
  `src/platform/win32/` 안에 있었고, 이름과 내용이 어긋난 채 Linux가 그것을 컴파일했다.**
* `src/platform/win32/`: Win32 백엔드 — 3a~3d 계층의 Windows 구현(폴트 전달, 가상 메모리,
  워커 신호, 안전한 메모리 복사, 환경 변수, 자식 프로세스)
* `src/platform/linux/`: Linux 전용 세부 구현 중 i386·x86-64 공용 부분. 아키텍처별 부분은 `x86/`(i386 machine context, FSAVE, i386 thunk·스택 전환)과 `x64/`(x86-64 machine context, FXSAVE, R15 guest ESP, long-mode thunk·guest entry, x64 fault 진단)에 있고 CMake가 포인터 크기로 고릅니다. x64 공개 헤더(`linux_x64_aot_dispatch.h`, `linux_x64_aot_frame.h`, `linux_x64_guest_entry.h`, `linux_x64_guest_registers.h`)는 같은 규칙으로 `include/repiu/platform/linux/x64/`에 있습니다(Task 757)
* `src/platform/web/`: Web 전용 세부 구현

현재 추가되는 디렉터리:

* `include/repiu/exe/`: MZ/LE 실행 파일 분석용 공용 헤더
* `include/repiu/hle/`: 정적 HLE profile과 서비스 범위 공용 헤더
* `include/repiu/target/`: 정적 target profile과 target registry 공용 헤더
* `scripts/`: 반복 검증용 빌드 스크립트
* `src/exe/`: MZ/LE 파서 구현
* `src/hle/`: 정적 HLE profile 등록 구현
* `src/engine/`: 실행 메모리 정책과 실행 backend 구현 위치 (플랫폼 공용)
* `src/target/`: 정적 target profile 등록 구현
* `src/tools/exe_analyzer/`: 비실행 콘솔 분석 도구

예정된 주요 모듈:

* `TargetRegistry`: 게임 타깃과 버전 선택. 정적 C++ 등록 구조는 `dos4gw_hello`와 MAME
  `xtom3d.cpp` 순서의 PIU profile 22개(`pumpit1`~`pumpipx3b`)를 제공합니다.
  `piu_1st`는 제거됐으며 실행기·supervisor·analyzer의 기본 게임 target은 `pumpit1`입니다.
  모든 PIU MAME CHD profile은 공용 PIU CHD mount와 `piu_common` HLE 경로를 재사용하고
  JAMMA/YMZ280B/EEPROM board capability를 활성화합니다. PIU10 ISA board와 CAT702은
  독립 capability이며 `pumpito` 이후 세대 profile에서 둘 다 활성화됩니다.
* `TargetProfile`: 실행 파일 경로, 작업 디렉터리, 자산 루트, 포맷 힌트, HLE 프로파일 id, 버전별 메타데이터와 하드웨어 capability
* `HleProfileRegistry`: target이 참조하는 HLE 프로파일 선택. 현재 단계에서는 정적 C++ 등록 구조로 `piu_common`을 제공한다.
* `HleProfile`: target이 요구하는 DOS/DPMI/하드웨어 HLE 서비스 범위
* `ExecutableReader`: 원본 실행 파일과 관련 파일을 읽는 공용 파일 입력 계층
* `MzParser`: DOS MZ 헤더와 LE/LX 위치 파악
* `Dos4gwExecutableLoader`: target profile을 기준으로 MZ/LE 파싱, 이미지 매핑, fixup 분석, 내부 relocation dry-run을 하나의 load result로 묶는다.
* `LeParser`: LE 헤더, 오브젝트 테이블, 페이지 테이블, fixup page table, fixup record table 범위, 1차 fixup record 디코딩, 엔트리 포인트 파싱
* `ImageMapper`: 원본 보호 모드 코드가 기대하는 메모리 이미지 구성. 현재 단계에서는 `Dos4gwExecutableLoader`를 통해 내부 relocation dry-run, skipped source 관찰 정보, full source type 분포를 제공한다.
* `RuntimeMemory`: 실행 메모리, 스택, 힙, selector 추상화, HLE 영역 관리. 현재 단계에서는 실행 메모리를 할당하지 않고 object region, entry, stack top, HLE reserve base를 계산하는 dry-run을 제공한다.
* `Win32RuntimeMemoryPolicy`: Win32 host pointer size, 32-bit direct execution 가능 여부, preferred allocation base, reserve size를 보고한다. 현재 단계에서는 실제 메모리를 할당하지 않는다.
* `Win32 x86 Build`: 원본 32-bit x86 entry 직접 실행을 준비하기 위해 `scripts/build_win32_x86.bat`로 `build\win32_x86_debug` multi-config 트리를 생성하고 검증한다. `-Configuration` 인자로 구성을 고르며 `scripts/build_win32_x86_release.bat`이 Release 진입점이다. 정확성 검증은 Debug, 성능 측정은 Release로 나눈다(Task 330의 Debug 계수 11.34배와 단계 순위 역전).
* `ExecutionEngine`: 원본 32-bit x86 코드로 제어 이전
* `HleDispatcher`: DOS, DPMI, 타이머, 입력, 그래픽, 오디오, 파일 시스템 호출 처리
* `TraceLogger`: 로더 결정, HLE 호출, 예외, 실행 단계 기록
* `LeResidentName`: LE resident-name/ordinal과 decorated `@N` 인자 크기를 asset에서 복원한다.
* `VirtualGlideModule`: `glide2x.ovl`의 hardware code를 실행하지 않고 LINEXE handle과 asset-validated export gate를 제공한다.
* `GlideGateDispatcher`: `{linear address, client CS}` procedure pointer를 제공하고 ordinal별 trap에서 guest stdcall ABI를 해석한다. 현재 init/query/select까지 관찰 기반 최소 의미를 제공하며 `grSstWinOpen` presentation 정책이 다음 경계다.
* `GlideHle` (`include/repiu/hle/glide_hle.h`, `src/hle/glide_hle.cpp`): asset-derived export registry, ordinal gate image, decorated argument size, resolution decoding과 플랫폼 공용 논리 상태를 담당한다.
* `Win32GlideOpenGlBackend` (`include/repiu/engine/glide_opengl_backend.h`, `src/engine/glide_opengl_backend.cpp`): SDL3 resizable host window, OpenGL context, texture cache, GLSL combine/perspective/fog, LFB presentation과 event pump를 담당한다. Glide 논리 해상도는 640×480으로 유지하고 host window는 기본 2배이며 `Alt+1..4`로 1–4배를 선택한다. 실행기 메인 스레드가 SDL video/GL을 소유하고 게스트 작업 스레드의 Glide 호출은 동기 명령 큐로 전달된다.
* `Win32ExecutionTrampoline`은 Glide 구현을 직접 소유하지 않고 guest stack/register ABI를 공용 Glide HLE와 platform backend에 연결한다.
* `GlideSignatureCatalog`: 실제 관찰된 API의 stack byte count와 void/EAX/x87 반환 kind를 중앙에서 관리하며 asset `@N`과 교차 검증한다.
* `GlideLogicalState`는 lazy OpenGL texture object와 독립적으로 8 MiB virtual TMU 범위(`0..0x007FFFF8`)와 LFB pixel-format 상태를 보존한다.
* `GlideLfb` (`include/repiu/hle/glide_lfb.h`, `src/hle/glide_lfb.cpp`): LFB staging surface, `GrLfbInfo_t` 직렬화, color-format 인식 565↔RGBA8 변환을 담당하는 플랫폼 공용 계층이다. `grLfbLock`은 게스트가 네이티브 명령으로 직접 기록할 실제 주소를 건네므로, HLE 경계가 함수가 아니라 **메모리 표면**이 되는 유일한 Glide 경로다. staging buffer는 아레나가 아닌 호스트 소유 할당이며(게스트는 flat DS로 네이티브 실행), 64비트 host에서는 `GrLfbInfo_t::lfbPtr`이 32비트이므로 `GlideLfbGuestStorage`(Task 708)가 공용 `ReserveLowAddressMemory` 사다리로 4 GiB 아래에 두고 surface에 설치한다. surface 자체는 저장소를 배치하지 않고 받기만 해 `repiu::hle`가 플랫폼을 이름 부르지 않게 한다. 모든 lock에서 현재 framebuffer로 seeding해 write lock이 기존 화면을 지우지 않게 한다. ARGB/RGBA lock은 RGB565, ABGR/BGRA lock은 BGR565로 encode/decode하며 `grSstWinOpen`의 color format을 기본값으로, `grLfbWriteColorFormat` 상태를 write lock override로 사용한다.
* Task 726의 Linux x64 전용 `GlideLfbNativeStoreCensus`는 `REPIU_LINUX_X64_LFB_STORE_CENSUS=1|on|true`일 때 write lock이 노출한 staging 주소 범위에서만 native AOT write observer를 활성화합니다. 명시적으로 decode된 AOT store의 겹침 store 수와 byte 수를 관찰할 뿐, guest memory, lock/unlock 순서, decode/present를 바꾸지 않습니다. string/implicit store, decode 실패, 비-AOT 경로는 포함하지 않으므로 결과는 전체 guest write가 아닌 보수적 하한입니다.
* `GlideLfbRegion` (`include/repiu/hle/glide_lfb_region.h`, `src/hle/glide_lfb_region.cpp`): lock 없이 사각형을 옮기는 `grLfbWriteRegion`/`grLfbReadRegion`의 플랫폼 공용 계층이다. `GrLfbSrcFmt_t` 표(565=`0x00`, 555, 1555, 888, 8888), stride 규칙(0이면 `width * bpp`), 사각형 클립, 565 staging surface와의 픽셀 변환을 담당한다. `GrLfbSrcFmt_t`는 픽셀 크기만 정하고 채널 순서는 `grLfbWriteColorFormat`(기본값은 `grSstWinOpen`의 cFormat)이 정하므로, 변환은 source 색 형식으로 풀어 목적지 색 형식으로 싼다. region의 `y`는 origin 상대가 아니라 프레임 버퍼 native 행 번호이므로 lock과 달리 뒤집지 않는다. 두 gate는 staging surface를 프레임 버퍼 shadow로 공유하며, region이 아닌 gate 진입 전에 한 번만 present한다. 행마다 present하면 seeding되지 않은 화면을 덮어써 그림을 파괴하고 프레임당 전체 화면 왕복이 행 수만큼 발생한다. Task 761의 고정밀 표시 경로(`WriteGlideLfbRegionRgba8`)는 토글이 켜져 있을 때 staging과 나란히 RGBA8 shadow를 유지해 24/32-bit source가 565 양자화 없이 화면에 도달하게 한다. 565 staging은 계속 게스트 가시 진실이라 readback은 비트 동일하고, 16-bit source는 pack→decode 왕복과 같은 값을 shadow에 써서 토글이 그 표시 결과를 바꾸지 못한다. 토글은 `REPIU_GLIDE_LFB_HIGH_PRECISION`(기본 on, `0|off|false`로 끔)과 in-game OSD가 제어한다.
* `MesaFxTextureSource` (`include/repiu/hle/mesa_fx_texture_source.h`, `src/hle/mesa_fx_texture_source.cpp`, issue #37): PIU 실행 파일에 정적으로 링크된 Mesa 3.x fx 드라이버는 RGBA 텍스처를 ARGB4444로, RGB를 RGB565로 잘라 `grTexDownloadMipMapLevel`에 넘긴다(`docs/analysis/mesa-fx-texture-path.md`). 이 계층은 게이트 시점 레지스터(ESI=fx 텍스처 정보, EBP=Mesa level, `[ESP+0x34]`=호출자가 저장한 텍스처 객체)로 Mesa 객체를 따라가 원본 8bit 이미지를 읽고, Mesa와 같은 정수 배 확대로 Glide 크기에 맞춘 뒤 **다시 4444/565로 줄였을 때 게임 데이터와 바이트 단위로 같을 때만** 돌려준다. 게스트 메모리는 읽기만 한다. 백엔드는 그런 텍스처의 두 이미지를 모두 보관하고 옵션(`REPIU_GLIDE_TEXTURE_FULL_PRECISION`, 기본 on, OSD·런처 설정)에 따라 올리며, 옵션이 바뀌면 다음 이벤트 펌프에서 다시 올린다. 최종 로그 `Glide texture full precision used/...`가 결과를 센다.
* `GlideOsd` (`include/repiu/engine/glide_osd.h`, `src/engine/glide_osd.cpp`): 런처가 예고한 인게임 OSD의 첫 구현이다. Glide backend의 SDL 창 안에 Dear ImGui로 그리며 `Tab`으로 열고 닫는다(Tab은 게임 입력에 전달되지 않고, 다른 입력은 OSD가 열려 있어도 게임으로 계속 흐른다. issue #55부터 게임패드 입력은 예외로, 열려 있는 동안 OSD를 조작하고 게임에는 가지 않는다). 현재 항목은 LFB 고정밀 토글 체크박스 하나로, backend의 atomic을 직접 읽고 쓴다. 모든 호출은 host 스레드에서 오고, buffer swap 직전에 렌더되며 dummy mode에서는 만들지 않는다. Task 768부터 화면 shader 메뉴(콤보, Reload, `#pragma parameter` 슬라이더)도 담는다. #5부터 맨 위에 Renderer 절을 둔다: backend가 GL context를 만든 직후 `glGetString`(renderer, vendor, version)과 `SDL_GetCurrentVideoDriver()`로 GL 없는 값 구조체 `GlRendererIdentity`(`include/repiu/engine/gl_renderer_identity.h`)를 채워 넘기고, `IsSoftwareGlRenderer`(llvmpipe, softpipe, swrast, GDI Generic, WARP, SwiftShader의 대소문자 무시 부분 일치)가 참이면 경고 색으로 표시한다. OSD는 값을 보관만 해 GL 호출을 더하지 않는다. #15부터 re2DJ OSD와 같은 형태다: 화면 맨 위 (0,0)에 가로 전체 폭, 높이는 내용에 맞춤, 첫 줄은 `rePIU v{버전} ({빌드 라벨}) - Build {날짜}`, 둘째 줄은 `Target Profile : {id}`(loader가 실행 전에 `session_identity`에 한 번 쓴 값). 글자와 여백은 `imgui_ui_scale`의 `UiScaleForHeight`(OSD 규칙: 기준 480, 계수 0.75, 최소 0.75)로 창 높이에 비례하고, 폰트는 ImGui 1.92.9의 ProggyForever(`AddFontDefaultVector`)다. issue #45부터 정보 줄 아래 맨 위에 "Fullscreen"과 "Keep aspect ratio" 체크박스를 둔다. OSD는 present 도중 그려지므로 backend가 채워 넘긴 `GlideOsdDisplayOptions`에 바뀐 값만 남기고, backend가 다음 이벤트 펌프에서 적용한다. 다른 OSD 항목과 달리 이 둘은 `cfg/repiu.ini`에 저장된다.
* `GlidePostProcess` (`include/repiu/engine/glide_post_process.h`, `src/engine/glide_post_process.cpp`)와 GL 없는 `PostShaderSource`·`PostShaderCatalog` (`post_shader_source.*`, `post_shader_catalog.*`): Task 768의 표시 단계 후처리다. buffer swap에서 진단 샘플링 뒤·OSD 앞에, 막 front가 된 offscreen buffer의 텍스처를 선택된 shader로 창 framebuffer에 그린다(issue #69부터 복사 없음, RGB만 기록, Glide 상태는 backend가 쥔 값으로 복원). 사용자 shader는 호환 문맥용 GLSL 1.10 문체로 쓰여 있어 `AdaptLegacyGlslStage`가 단어 단위로 3.30 core / ES 3.00으로 옮긴다. 게스트 가시 상태는 바뀌지 않고, 기본 `none`이면 GL 호출이 없다. shader는 libretro 단일 pass GLSL의 부분집합이며 `InputSize`/`TextureSize`는 게임 논리 해상도다. 내장 `crt`·`scanline`은 `src/engine/post_shaders/*.glsl`을 CMake가 생성 헤더로 넣고, 사용자 파일은 `shaders/*.glsl`(작업 디렉터리 → 실행 파일 옆)에서 읽는다. 선택은 `REPIU_POST_SHADER` → 런처의 `[Video] post_shader` → `none`이며, 알 수 없거나 컴파일되지 않는 shader는 `none`으로 실행한다.
* Glide 텍스처 크기 규약(`GrLOD_t`는 열거값이며 `GR_LOD_256`=0)은 `docs/kb/glide-texture-lod-and-formats.md`에서 관리한다. `grTexTextureMemRequired`의 반환값은 게스트가 자기 TMU 배치를 결정하는 입력이므로 정확성이 게스트 동작에 직접 전파된다.
* `Win32X87Context` (`include/repiu/engine/x87_context.h`, `src/engine/x87_context.cpp`): SEH `CONTEXT`의 x87 TOP/tag/80-bit register를 갱신하여 guest float 반환을 독립적으로 처리한다.

## Planned Structure

Planned shared structure:

* `include/`: project C++ headers intended for external inclusion
* `src/`: platform-neutral loader and runtime core
* `src/engine/`: the platform-neutral execution engine -- AOT/DBT, boundary handling, the execution
  trampoline, telemetry, instruction emulation, the DOS/BIOS/port-I/O HLE, and Glide. **Before Task
  520 this lived under `src/platform/win32/`, where its name contradicted its contents and Linux
  compiled it anyway.**
* `src/platform/win32/`: the Win32 backend -- the Windows implementations of the 3a-3d layer (fault
  delivery, virtual memory, worker signals, safe memory copy, environment, child processes)
* `src/platform/linux/`: the Linux-specific details shared by i386 and x86-64. Per-architecture parts live in `x86/` (i386 machine context, FSAVE, i386 thunks and stack switch) and `x64/` (x86-64 machine context, FXSAVE, R15 as guest ESP, long-mode thunks and guest entry, x64 fault diagnostics), chosen by CMake by pointer size. The x64 public headers (`linux_x64_aot_dispatch.h`, `linux_x64_aot_frame.h`, `linux_x64_guest_entry.h`, `linux_x64_guest_registers.h`) follow the same rule in `include/repiu/platform/linux/x64/` (Task 757)
* `src/platform/web/`: Web-specific details

Directories added now:

* `include/repiu/exe/`: public headers for MZ/LE executable analysis
* `include/repiu/hle/`: public headers for static HLE profiles and service scopes
* `include/repiu/target/`: public headers for static target profiles and target registry
* `scripts/`: build scripts for repeated verification
* `src/exe/`: MZ/LE parser implementation
* `src/hle/`: static HLE profile registration implementation
* `src/engine/`: executable memory policy and the execution backends (platform-neutral)
* `src/target/`: static target profile registration implementation
* `src/tools/exe_analyzer/`: non-executing console analysis tool
* `src/host/loader/`: the loader application entry point, shared by the Win32 x86 and Linux hosts (moved from `src/host/win32/` in Task 731). This is the practical loader path that selects a target, loads the DOS/4GW executable, builds a relocated image, places it in process memory, and performs the current minimal execution attempt.
* `src/host/win32/`: Win32-only entry points (the supervisor).
* `src/host/linux/`: the Linux launcher entry point.

Planned major modules:

* `TargetRegistry`: game target and version selection. Static C++ registration provides
  `dos4gw_hello` and 22 PIU profiles (`pumpit1` through `pumpipx3b`) in MAME
  `xtom3d.cpp` order. `piu_1st` has been removed, and the runner, supervisor, and analyzer
  default to `pumpit1`. Every MAME CHD profile reuses the shared PIU CHD mount and
  `piu_common` HLE path and enables the JAMMA/YMZ280B/EEPROM board capability. PIU10 ISA and
  CAT702 remain independent capabilities enabled on the generation beginning with `pumpito`.
* `TargetProfile`: executable path, working directory, asset root, format hint, HLE profile id, optional ROM-set id, version metadata, and target-specific early runtime reservation hints
* `HleProfileRegistry`: HLE profile selection referenced by targets. The current step provides `piu_common` through static C++ registration.
* `HleProfile`: DOS/DPMI/hardware HLE service scope required by a target
* `ExecutableReader`: shared file input layer for original executables and related files
* `MzParser`: DOS MZ header and LE/LX location
* `Dos4gwExecutableLoader`: groups MZ/LE parsing, image mapping, fixup analysis, and internal relocation dry-run into one load result based on the target profile.
* `LeParser`: LE header, object table, page table, fixup page table, fixup record table range, first-pass fixup record decoding, and entry point parsing
* `ImageMapper`: memory image expected by the original protected-mode code. The current step provides internal relocation dry-run, skipped source observability, and full source type distribution through `Dos4gwExecutableLoader`.
* `RuntimeMemory`: executable memory, stack, heap, selector abstraction, and HLE regions. The current step provides a dry-run that calculates object regions, entry, stack top, and HLE reserve base without allocating executable memory.
* `RuntimeMemoryArenaPlan`: shared runtime arena reserve planning. It combines the current image reserve size with observed expansion slack so DOS/4G post-resize writes can be backed by host memory. The Win32 loader uses this reserve size for relocated-base probing and image placement.
* The Win32 traced memory-store path keeps narrowly bounded shadow memory for observed allocator failure metadata outside the committed arena. A `C7 /0` dword sentinel store of `0xFFFFFFFF` is accepted only within 1 MiB after the arena end; unrelated out-of-arena stores remain fatal so missing mappings are not hidden.
* An `89 /r` store may extend that allocator shadow only when the first header value exactly equals the distance to the existing sentinel, or when a later field falls inside or immediately after the resulting shadow range. This preserves allocator metadata relationships without turning shadow memory into a general substitute for missing guest mappings.
* Objects whose base register is inside the final 64 bytes of the arena may preserve `C7 /0`, `89 /r`, and `D9 /2-/3` fields that cross no more than 64 bytes beyond arena end. The base must remain inside real guest memory; objects whose base is already outside the arena are not covered by this boundary-tail policy.
* A contiguous object array may continue that boundary tail when each next base exactly matches the recorded frontier. Its span is derived from the observed `ESI` count times `EDX` stride, accepted only between 64 bytes and 1 MiB, and otherwise falls back to 4 KiB. An explicit `REPIU_EXECUTION_TIMEOUT_MS` budget remains the final observation bound; since Task 435 the default is unlimited, so a run that needs one states it.
* An unprefixed `8B /r` low-memory read inside the first 4 KiB uses zero-backed DOS memory after real-arena and shadow-memory misses when a guest `DS` selector is active. It does not add the relocated image base to a guest low-memory offset.
* A faulting `03 /r` with a complete dword source in shadow memory performs the 32-bit ADD on the ModRM-selected register and restores `CF/PF/AF/ZF/SF/OF`; mapped-memory ADD remains on the direct CPU path.
* A faulting `83 /1` no-SIB dword destination in shadow memory performs OR as a read-modify-write, records the resulting store, clears `CF/OF`, updates `PF/ZF/SF`, and preserves undefined `AF`.
* A faulting `38 /r` no-SIB byte source in shadow memory compares it with the ModRM-selected legacy byte register and restores `CF/PF/AF/ZF/SF/OF` without modifying either operand.
* The allocator probe at relocated offset `0xF7A71` records only the confirmed request sizes `0x2C` and `0x1008`. The header OR at `0xF7AD4` then binds that request to a block and exposes only `[block+4, block+size-4)` as zero-backed shadow payload. Explicit shadow bytes override zero backing; unconfirmed sizes and unrelated holes remain faults.
* `RelocatableRuntimeImage`: primary execution memory direction after fixed low-address reservation proved unreliable in Win32 x86. This subsystem maps original LE object bases to a safe runtime base, reapplies LE relocation records for the new addresses, and calculates relocated entry and stack addresses while preserving original game code. The current dry-run uses `0x01000000` as the relocated image base and does not allocate or write executable memory.
* `RelocatedRuntimeImage`: materialized C++ buffer form of the relocatable plan. It copies each mapped LE object into owned buffers and writes supported 32-bit internal relocation values for the relocated base. This still does not allocate executable OS memory or call original code.
* `Win32RelocatedImagePlacement`: Win32 process-memory placement for relocated image buffers. It reserves and commits the relocated image range, copies object buffers, applies minimal object protection from LE flags, and releases the range without transferring control to original code.
* `Win32MinimalExecutionTrampoline`: first observation-only execution path. It calls the relocated entry from a separate thread, catches SEH exceptions, and reports return/exception/timeout. It does not yet switch to the guest stack or dispatch HLE traps.
* `Win32ExecutionSnapshot`: grouped x86 guest register observation state used by the Win32 execution path. Timeout handling exposes a `timeout_snapshot` output slot and reports whether it was captured. Forced `SuspendThread`/`GetThreadContext` capture remains disabled because it can hang the loader; a safer sampling watcher or HLE-routed trace should fill this structure.
* `Win32ExceptionDispatchLiveness`: the vectored exception route atomically counts valid handler entries and RAII-guarded exits and records the last entry EIP. It does not suspend the guest or alter dispatch. An outstanding count of one at quiet timeout proves that polling expired while one handler invocation had not returned; equal counts prove no dispatch remained active.
* `Win32AllocatorProbeObservation`: a diagnostic-only 16-entry ring for exact relocated offset `0xF7A71`. It records EAX, ESI/decoded source, DS, pending allocation state before/after, and the handling result. Sequence numbers preserve chronological output after overwrite without allowing trace memory to grow.
* `Win32AllocatorControlFlowObservation`: a diagnostic-only 32-entry ring for exception entries in relocated allocator range `[0xF7A71, 0xF7AD5)`. It records exception code, four opcode bytes, core allocator registers, flags, and pending state without modifying dispatch. The range exposes free-list traversal and metadata update paths while keeping storage bounded.
* `Win32ShadowWriteProvenance`: an allocation-free 256-entry ring that records recent shadow writes and correlates complete dword allocator reads with a writer when one retained write covers all four bytes. A dynamic `unordered_map` prototype was rejected after causing `0xC0000374` heap corruption in the exception path.
* `DosLowMemory`: shared fixed 64 KiB DOS low-memory backing with checked little-endian byte/word/dword reads and up-to-dword writes. It starts zeroed and does not synthesize IVT, BIOS, environment, extender-private, or allocator state.
* `SelectorTable` now provides checked selector base/limit translation. Observed segment loads provisionally register a present base-zero, 64 KiB descriptor until DPMI descriptor lifecycle services are modeled. Win32 FS-word and generic DS low-memory reads use this translation instead of an implicit numeric-offset zero fallback.
* `Win32SegmentLoadObservation`: a diagnostic-only 16-entry ring for segment-load HLE events. It records relocated EIP, target segment register, selector, and source, exposing the stable PIU sequence that loads DS/FS selector `0x2C` from image address `0x021A664D`.
* `Win32RuntimeMemoryPolicy`: reports Win32 host pointer size, 32-bit direct execution support, preferred allocation base, and reserve size. The current step does not allocate memory.
* `Win32AddressRangeProbe`: checks the required Win32 runtime address range with `VirtualQuery` before any allocation. It reports whether the fixed DOS/4GW image range is free and records the first blocking memory block when it is occupied. This is a dry-run only and does not reserve executable memory.
* `Win32AddressRangeReservation`: attempts to reserve the fixed original runtime address range with `VirtualAlloc(MEM_RESERVE)` and reports success or the Windows error code without executing original code.
* `Win32HostImageBasePolicy`: configures 32-bit Win32 executable targets so the host image base stays outside both the original DOS/4GW fixed image range and the relocated image range. The current baseline applies `/BASE:0x10000000` and `/DYNAMICBASE:NO` to Win32 x86 host targets.
* `Win32LoaderApp`: dedicated Win32 loader executable target named `repiu`. It owns the current loader orchestration path: target selection, executable read, DOS/4GW load, relocated image planning, relocated buffer creation, Win32 process-memory placement, and minimal execution trampoline invocation. In addition to built-in target ids, it can accept a direct DOS/4GW executable path and run it through a temporary `direct_executable` target using the `dos4gw_console_sample` HLE profile. This keeps local sample testing from adding one target profile per executable.
* `DosVirtualFileSystem`: shared HLE state for DOS path resolution and file handles. It treats the target profile working directory as the virtual DOS drive root, keeps a virtual current directory without changing the host process current directory, and handles `INT 21h AH=0x3B` chdir, `AH=0x3D` open, and — since Task 477 — `AH=0x3C` create and `AH=0x40` writes to the handles create returns. Handles are allocated lowest-free from 5 (Task 228) and cache their host stream (Task 374) on both the read and write side. Created files land under the mount root at the guest's own path and therefore vanish when the mount cache is invalidated and re-extracted, which suits the log files observed so far; the creation attribute is recorded in the state's attribute overrides rather than stamped on the host file, because a host read-only attribute would block the writes the returned handle exists for. `AH=0x40` against any other handle, standard handles 0-4 included, keeps its console path.
* `Win32SegmentMemoryLoadHle`: observation-driven segment override memory access HLE in the Win32 execution trampoline. The current scope handles the traced `26 8A 4F FF` byte load as `ES:[EDI - 1]`, returns the DOS command tail length byte for `ES:0x80`, and records the handled segment memory load in the execution attempt.
* `Win32 x86 Build`: prepares direct original 32-bit x86 entry execution by generating and verifying the multi-config `build\win32_x86_debug` tree through `scripts/build_win32_x86.bat`. A `-Configuration` parameter selects the configuration and `scripts/build_win32_x86_release.bat` is the Release entry point; correctness verification stays on Debug while every performance number comes from Release, because Task 330 measured an 11.34x Debug factor that inverts the stage ranking.
* `OpenWatcomLocalSampleSuite`: script-driven compatibility report for local OpenWatcom samples. `scripts/build_openwatcom_samples.ps1` enumerates every source in the selected local `tools/openwatcom/samples/clibexam` and `tools/openwatcom/samples/cplbexam` suites, applies a DOS/4GW console build plan for known option-sensitive samples, explicitly marks samples outside that target as skipped, builds the remaining sources into Git-excluded `build/openwatcom_samples/`, and writes a stable manifest. The script can also skip the Win32 host rebuild with `-SkipHostBuild` when only sample build-plan validation is needed. `scripts/test_openwatcom_samples.ps1` then runs the manifest's built EXEs through the loader direct path, writes an HTML pass-rate report plus summary JSON under `build/openwatcom_sample_report/`, and can compare against or update the Git-tracked baseline at `tests/baselines/openwatcom_samples.json`. Baseline updates also append dated, versioned milestone JSON and matching HTML reports under `tests/history/openwatcom_samples/` for future dashboard and inspection use. OpenWatcom sample sources and EXEs are not committed because their license is not the project default BSD 3-Clause baseline.
* `ExecutionEngine`: control transfer to original 32-bit x86 code
* `HleDispatcher`: DOS, DPMI, timer, input, graphics, audio, and filesystem calls
* `TraceLogger`: loader decisions, HLE calls, exceptions, and execution milestones
* `LeResidentName`: recovers LE resident names, ordinals, and decorated `@N` argument sizes from the user asset.
* `VirtualGlideModule`: exposes a LINEXE handle and asset-validated export gates without executing `glide2x.ovl` hardware code.
* `GlideGateDispatcher`: returns `{linear address, client CS}` procedure pointers and decodes guest stdcall ABI at ordinal traps. Observation-backed init/query/select semantics are present; `grSstWinOpen` presentation policy is the next boundary.
* `GlideHle` (`include/repiu/hle/glide_hle.h`, `src/hle/glide_hle.cpp`) owns the asset-derived export registry, ordinal gate image, decorated argument sizes, resolution decoding, and platform-neutral logical state.
* `Win32GlideOpenGlBackend` (`include/repiu/engine/glide_opengl_backend.h`, `src/engine/glide_opengl_backend.cpp`) owns the SDL3 resizable host window, OpenGL context, texture cache, GLSL combine/perspective/fog, LFB presentation, and event pump. Glide remains logically 640×480, while the host window defaults to 2× and `Alt+1..4` selects 1×–4×. The executor main thread owns SDL video/GL, and synchronous commands carry guest-worker Glide calls to it.
* `Win32ExecutionTrampoline` does not own Glide implementation details; it connects guest stack/register ABI to shared Glide HLE and the platform backend.
* `GlideSignatureCatalog` centrally records observed API stack-byte counts and void/EAX/x87 return kinds, cross-checked against asset `@N` metadata.
* `GlideLogicalState` exposes an 8 MiB virtual TMU range (`0..0x007FFFF8`) independently of lazy OpenGL texture objects and retains LFB pixel-format state.
* `GlideLfb` (`include/repiu/hle/glide_lfb.h`, `src/hle/glide_lfb.cpp`) is the platform-neutral LFB layer: staging surface, `GrLfbInfo_t` serialization, and color-format-aware 565↔RGBA8 conversion. `grLfbLock` hands the guest a real address it writes with native instructions, making this the one Glide path whose HLE boundary is a **memory surface** rather than a function. The staging buffer is a host-owned allocation rather than an arena carve (the guest executes natively under a flat DS). Because `GrLfbInfo_t::lfbPtr` is 32 bits, `GlideLfbGuestStorage` (Task 708) places it below 4 GiB on a 64-bit host through the shared `ReserveLowAddressMemory` ladder and installs it into the surface; the surface accepts storage but places none, which keeps `repiu::hle` from naming an operating system. It is seeded from the current framebuffer on every lock so a write lock never erases existing content. ARGB/RGBA locks encode and decode RGB565, while ABGR/BGRA locks use BGR565; `grSstWinOpen` supplies the default and `grLfbWriteColorFormat` overrides write-lock state.
* Linux x64-only `GlideLfbNativeStoreCensus` from Task 726 activates the native AOT write observer only while a write lock exposes its staging-address range, when `REPIU_LINUX_X64_LFB_STORE_CENSUS=1|on|true`. It observes overlapping store and byte counts for explicitly decoded AOT stores only; it changes neither guest memory nor lock/unlock ordering nor decode/present. String/implicit stores, decode failures, and non-AOT paths are excluded, so the result is a conservative lower bound rather than all guest writes.
* `GlideLfbRegion` (`include/repiu/hle/glide_lfb_region.h`, `src/hle/glide_lfb_region.cpp`) is the platform-neutral layer for the lock-free rectangle transfers `grLfbWriteRegion` and `grLfbReadRegion`: the `GrLfbSrcFmt_t` table (565 = `0x00`, 555, 1555, 888, 8888), the stride rule (0 means `width * bpp`), rectangle clipping, and conversion against the 565 staging surface. `GrLfbSrcFmt_t` fixes only the pixel size; the channel order comes from `grLfbWriteColorFormat` (defaulting to `grSstWinOpen`'s cFormat), so conversion unpacks in the source format and packs in the destination one. Region `y` is a native frame buffer row rather than an origin-relative one, so unlike a lock it never flips. Both gates share the staging surface as a frame buffer shadow presented once before the next non-region gate; presenting per row would blit an unseeded screen over the picture and cost one full-screen round trip per scanline. Task 761's high-precision presentation path (`WriteGlideLfbRegionRgba8`) keeps an RGBA8 shadow beside the staging while its toggle is on, so 24/32-bit sources reach the screen without 565 quantization. The 565 staging stays the guest-visible truth, so readbacks are bit-identical, and 16-bit sources write the pack→decode round-trip values into the shadow so the toggle cannot change their displayed result. The toggle is controlled by `REPIU_GLIDE_LFB_HIGH_PRECISION` (on by default, off with `0|off|false`) and the in-game OSD.
* `MesaFxTextureSource` (`include/repiu/hle/mesa_fx_texture_source.h`, `src/hle/mesa_fx_texture_source.cpp`, issue #37): the Mesa 3.x fx driver linked into the PIU executables cuts RGBA textures to ARGB4444 and RGB to RGB565 before `grTexDownloadMipMapLevel` (`docs/analysis/mesa-fx-texture-path.md`). This layer follows the gate-time registers (ESI the fx texture info, EBP the Mesa level, `[ESP+0x34]` the texture object the caller saved) to the original 8-bit image, enlarges it to the Glide size by Mesa's whole factors, and returns it **only when truncating it again reproduces the game's data byte for byte**, reading guest memory only. The backend keeps both images of such a texture and uploads the one the option selects (`REPIU_GLIDE_TEXTURE_FULL_PRECISION`, on by default, OSD and launcher setting), re-uploading on the next event pump after a change; the final log line `Glide texture full precision used/...` counts the outcomes.
* `GlideOsd` (`include/repiu/engine/glide_osd.h`, `src/engine/glide_osd.cpp`) is the first realization of the in-game OSD the launcher section promised: Dear ImGui drawn inside the Glide backend's SDL window, opened and closed with `Tab` (Tab never reaches game input; other input keeps flowing to the game while the overlay is open, except gamepad input since issue #55, which drives the overlay instead). Its one control is the LFB high-precision checkbox, reading and writing the backend's atomic directly. Every call arrives on the host thread, it renders just before the buffer swap, and it is not created in dummy mode. Since Task 768 it also holds the screen shader menu (combo, Reload, `#pragma parameter` sliders). Since #5 it opens with a Renderer section: right after creating the GL context the backend fills the GL-free `GlRendererIdentity` (`include/repiu/engine/gl_renderer_identity.h`) from `glGetString` (renderer, vendor, version) and `SDL_GetCurrentVideoDriver()` and hands it over, and `IsSoftwareGlRenderer` (a case-insensitive substring match for llvmpipe, softpipe, swrast, GDI Generic, WARP and SwiftShader) shows it in a warning colour. The OSD only keeps the values and adds no GL call. Since #15 it takes re2DJ's OSD layout: at (0,0) across the full width of the screen, as tall as its content, opening with `rePIU v{version} ({build label}) - Build {date}` and `Target Profile : {id}` (written once by the loader into `session_identity` before execution). Text and spacing follow the window height through `UiScaleForHeight` in `imgui_ui_scale` (OSD rule: reference 480, factor 0.75, minimum 0.75), and the font is ImGui 1.92.9's ProggyForever (`AddFontDefaultVector`). Since issue #45 "Fullscreen" and "Keep aspect ratio" checkboxes sit first below the info lines. The OSD draws in the middle of a present, so it only leaves a change in the `GlideOsdDisplayOptions` the backend filled, and the backend applies it at the next event pump. Unlike the other OSD items these two are stored in `cfg/repiu.ini`.
* `GlidePostProcess` (`include/repiu/engine/glide_post_process.h`, `src/engine/glide_post_process.cpp`), with the GL-free `PostShaderSource` and `PostShaderCatalog` (`post_shader_source.*`, `post_shader_catalog.*`), is Task 768's presentation-only post-processing. At the buffer swap, after the diagnostic sampling and before the OSD, it draws the texture of the offscreen buffer that has just become front into the window's framebuffer through the selected shader (no copy since issue #69, RGB only, Glide state put back from what the backend holds). User shaders are written in the GLSL 1.10 style of compatibility contexts, and `AdaptLegacyGlslStage` carries them word by word to 3.30 core / ES 3.00. Guest-visible state does not change, and the default `none` issues no GL call. Shaders are a subset of libretro single-pass GLSL with `InputSize`/`TextureSize` set to the game's logical resolution. The built-in `crt` and `scanline` are `src/engine/post_shaders/*.glsl` embedded by CMake through a generated header; user files come from `shaders/*.glsl` (working directory, then next to the executable). Selection is `REPIU_POST_SHADER`, then the launcher's `[Video] post_shader`, then `none`; an unknown or uncompilable shader runs as `none`.
* Glide texture sizing rules (`GrLOD_t` is an enumeration with `GR_LOD_256` = 0) are maintained in `docs/kb/glide-texture-lod-and-formats.md`. `grTexTextureMemRequired`'s return value is an input to the guest's own TMU layout, so its accuracy propagates directly into guest behavior.
* `Win32X87Context` (`include/repiu/engine/x87_context.h`, `src/engine/x87_context.cpp`) independently updates x87 TOP, tags, and 80-bit registers in an SEH `CONTEXT` for guest float returns.

## 갱신 규칙

* 코드 구조가 추가되거나 변경되면 이 문서를 같은 작업 단위에서 갱신한다.
* 플랫폼 공용 구조와 플랫폼별 세부 구조를 분리해서 기록한다.
* 임시 구현이라도 후속 정리 방향을 적는다.

## Update Rules

* Update this document in the same task unit whenever code structure is added or changed.
* Record platform-neutral structure separately from platform-specific details.
* Even for temporary implementation, record the intended follow-up direction.

## Glide GLSL renderer boundary

`GlideLogicalState`는 원본 Glide enum과 초기 raster state를 플랫폼 중립적으로 보존합니다. `GlideOpenGlBackend`는 메인 스레드 소유 SDL3 window/OpenGL context와 상태 변환을 담당하고, 별도 `GlideOpenGlShader`가 shader entry-point 해석, compile/link, program과 combine uniform을 소유합니다. 게스트 작업 스레드의 호출은 backend의 동기 명령 큐를 거쳐 메인 스레드에서 실행됩니다.

Glide 비교 함수 `0..7`은 backend에서 OpenGL `GL_NEVER..GL_ALWAYS`로 변환합니다. `grDepthBufferMode`가 depth test 활성 여부를 소유하고 `grDepthBufferFunction`은 비교 함수만 선택합니다. guest 반환 주소와 catalog signature가 검증된 뒤 specialized gate handler가 실패하면 공용 decline 경로가 보수적 반환값과 stdcall EIP/ESP 정리를 수행합니다. 반환 주소 불량과 signature 불일치는 ABI를 신뢰할 수 없으므로 hard reject로 유지합니다.

플랫폼 공용 `GlideImplementationIssueTracker`는 미구현 함수, 미지원 인자, backend 실패, ABI reject를 분류하고 함수·인자 조합별 반복 횟수를 합칩니다. 미구현과 미지원은 즉시 `[repiu-fatal]` 및 종료 시 `critical/FATAL`로 출력하지만, 검증된 stdcall ABI는 정상 정리 후 계속 실행합니다. 최대 128개 고유 record와 첫 8개 인자를 보존하며 분류별 total과 overflow는 별도로 누적합니다.

창 배율, 일반 resize, 전체화면 전환이 일어나면 Task 769의 `ComputeGlideLetterboxRect`가 논리 해상도의 비율을 유지하는 content rect를 drawable 가운데에 잡고, viewport·scissor·point size를 그 rect로 갱신합니다. 남는 띠는 present 직전에 검정으로 clear합니다. LFB readback은 content rect만 읽은 뒤 논리 해상도로 최근접 축소하여 원본 게스트의 LFB 크기와 row 순서를 보존하고, 띠가 게스트가 읽는 값에 섞이지 않게 합니다. 더블클릭 또는 `Alt+Enter`는 디스플레이 모드를 바꾸지 않는 테두리 없는 전체화면(`SDL_SetWindowFullscreen`, mode 미지정)과 창 모드를 오가며, 전체화면 중에는 `Alt+1..4`를 무시합니다. issue #45부터 사각형은 `ComputeGlidePictureRect(..., keep_aspect)`가 정합니다: 비율 유지(기본, `REPIU_GLIDE_KEEP_ASPECT`)면 위의 letterbox 사각형, 끄면 drawable 전체입니다. 다른 소비자는 모두 같은 `content_rect_`를 쓰므로 바뀐 곳은 `ApplyDrawableViewport` 하나입니다. 시작 시 전체 화면(`REPIU_GLIDE_FULLSCREEN`)은 배율 창을 만든 뒤 들어갑니다. OSD 체크박스·`Alt+Enter`·더블클릭으로 바뀐 값은 세션 sink(`include/repiu/engine/session_display_preferences.h`)로 loader에 알려지고, loader가 `cfg/repiu.ini`의 `[Video] fullscreen`·`keep_aspect`만 바꿔 저장합니다. 저장값을 시작 시 적용하는 것과 창이 닫힐 때 SDL이 전체 화면을 푸는 것은 알리지 않습니다. 엔진은 런처 타입을 모르고 sink가 없으면(probe·도구) 아무것도 쓰지 않습니다.

SDL 창 제목은 루트 `VERSION`에서 CMake가 검증·주입한 `REPIU_VERSION`, 빌드 identity 라벨(`Win/x86 Debug`처럼 플랫폼/아키텍처 빌드구성 — Task 738, `repiu::platform::BuildIdentityLabel`, 구성은 CMake의 `REPIU_BUILD_CONFIG="$<CONFIG>"`), backend 컴파일 날짜 `__DATE__`, 현재 `ExecutionBackend` 이름, 실측 FPS를 조합합니다. 실행 orchestration이 플랫폼 공용 backend enum을 `GlideOpenGlBackend`에 전달하며, backend는 성공한 guest buffer swap을 단조 시간 기준 약 1초 구간으로 집계합니다. 제목 생성과 갱신은 모두 SDL window를 소유한 실행기 메인 스레드에서 수행됩니다. `VERSION` 파일은 configure dependency이므로 변경 시 build system이 자동 재구성됩니다.

`SDL_EVENT_QUIT`과 `SDL_EVENT_WINDOW_CLOSE_REQUESTED`는 모두 host 종료 요청으로 기록됩니다. 실행기 polling loop가 요청을 감지하면 timeout과 동일한 context-recovery 절차로 guest worker를 먼저 멈추고, main thread에서 SDL/OpenGL과 translation worker를 정리합니다. event handler는 직접 process exit이나 resource 파괴를 수행하지 않습니다.

```mermaid
flowchart LR
    GUEST["Original Glide calls"] --> ABI["guest worker ABI"]
    ABI --> QUEUE["synchronous command queue"]
    QUEUE --> BACKEND["main-thread GlideOpenGlBackend"]
    BACKEND --> SHADER["GlideOpenGlShader"]
    BACKEND --> STREAM["GlideVertexStream"]
    BACKEND --> PAIR["GlideFramebufferPair<br/>front / back"]
    PAIR --> WINDOW["window framebuffer<br/>(present, post-process, OSD)"]
    SHADER --> GLSL["GL 3.3 core / ES 3.0 program"]
```

### GL 3.3 core / ES 3.0 부분집합 (issue #69)

렌더러는 OpenGL 3.3 core와 OpenGL ES 3.0(WebGL2)이 공유하는 부분집합만 씁니다. 네이티브(Win32·Linux)는 3.3 core 문맥을, 웹은 ES 3.0 문맥을 요청하며 무엇을 요청할지는 플랫폼의 `RequestedGlContext()`(`include/repiu/platform/host_gpu_driver.h`)가 정합니다. GLSL 머리말(`#version 330 core` / `#version 300 es`)은 만들어진 문맥이 정하므로 엔진에는 플랫폼 분기가 없습니다([설계](docs/design/20261016-i069-gles3-renderer.md)).

| 구성요소 | 파일 | 역할 |
|---|---|---|
| `GlApi`, `GlStateSnapshot` | `gl_api.{h,cpp}` | GL 1.1을 넘는 함수 표(SDL로 이름 조회), 문맥이 ES인지. 스냅샷은 드물게 쓰는 곳(크기 변경)만 |
| `GlslHeader`, `AdaptLegacyGlslStage` | `gl_shader_dialect.{h,cpp}` | 문맥별 머리말, 1.10 문체 후처리 shader의 단어 치환 |
| `GlideVertexStream` | `glide_vertex_stream.{h,cpp}` | 즉시 모드 대신 스트리밍 VBO·VAO. 꼭짓점은 즉시 모드가 받던 float 그대로 |
| `GlideFramebufferPair` | `glide_framebuffer_pair.{h,cpp}` | 그림 사각형 크기의 front/back FBO와 공용 깊이, 교환, 읽기, 창으로의 표시 |
| `GlUnitQuad`, `BuildGlProgram` | `gl_unit_quad.{h,cpp}` | 표시·후처리용 단위 사각형, 공용 attribute slot으로 program 만들기 |

투영(`glOrtho`), 점 크기, 알파 테스트(`glAlphaFunc`)는 결합 shader의 uniform과 `discard`입니다. 게임은 offscreen pair에만 그리고, `grBufferSwap`은 둘을 교환한 뒤 새 front를 창에 그립니다. 그래서 swap 뒤 back은 직전 front이고(Task 729가 기대는 page flip), WebGL이 표시 뒤 창 framebuffer를 지우거나 그리던 중인 프레임을 내보내도 게임 그림에 영향이 없습니다. 표시·LFB blit 뒤에는 backend가 쥔 게임 상태(`GameGlState`)로 되돌리며 GL에 묻지 않습니다(GL 조회는 스레드 드라이버를 멈춰 측정에서 프레임 처리량을 절반으로 떨어뜨렸습니다).

### 창 경계와 브라우저 페이지 (issue #72)

백엔드는 창·GL 문맥·스왑·크기·전체 화면·제목·스왑 간격을 `GlideHostWindow`(`include/repiu/engine/glide_host_window.h`)로
다룹니다. 구현은 SDL 창(`glide_host_window_sdl.cpp`, Win32·Linux·Node)과 브라우저 캔버스(`glide_host_window_browser.cpp`)이고,
플랫폼의 `BrowserCanvasAvailable()`(`include/repiu/platform/browser_host.h`)로 실행 중에 고릅니다. 브라우저 구현은 페이지가 Worker에
넘긴 OffscreenCanvas에 WebGL2 문맥을 만들고, GL 함수를 Emscripten으로 찾으며(`SetGlProcAddressLoader`), 스왑할 일이 없습니다.
단일 스레드 루프는 호스트 차례에서 화면 교체가 있었거나 16 ms가 지나면 `YieldToHostEventLoop()`로 페이지에 양보합니다
(브라우저 링크 타깃 `repiu_browser`만 Asyncify, [설계](docs/design/20261016-i072-browser-page.md)).

### The window boundary and the browser page (issue #72)

The backend handles the window, GL context, swap, size, fullscreen, title and swap interval through `GlideHostWindow`
(`include/repiu/engine/glide_host_window.h`). Its implementations are an SDL window (`glide_host_window_sdl.cpp`; Win32,
Linux, Node) and a browser canvas (`glide_host_window_browser.cpp`), chosen at run time by the platform's
`BrowserCanvasAvailable()` (`include/repiu/platform/browser_host.h`). The browser implementation makes a WebGL2 context on
the OffscreenCanvas the page handed the Worker, finds GL functions through Emscripten (`SetGlProcAddressLoader`) and has
nothing to swap. At the host turn the single-threaded loop yields to the page through `YieldToHostEventLoop()` once a
picture was swapped or 16 ms have passed (only the browser link target `repiu_browser` uses Asyncify,
[design](docs/design/20261016-i072-browser-page.md)).

### GL 3.3 core / ES 3.0 subset (issue #69)

The renderer uses only the subset OpenGL 3.3 core and OpenGL ES 3.0 (WebGL2) share. Native builds (Win32, Linux) ask for a 3.3 core context and the web for ES 3.0; what to ask for is the platform's `RequestedGlContext()` (`include/repiu/platform/host_gpu_driver.h`). The GLSL header (`#version 330 core` / `#version 300 es`) is picked by the context that was made, so the engine holds no platform branch ([design](docs/design/20261016-i069-gles3-renderer.md)).

| Component | Files | Role |
|---|---|---|
| `GlApi`, `GlStateSnapshot` | `gl_api.{h,cpp}` | the table of functions beyond GL 1.1 (looked up by name through SDL), whether the context is ES; the snapshot only where it is rare (resizing) |
| `GlslHeader`, `AdaptLegacyGlslStage` | `gl_shader_dialect.{h,cpp}` | the header per context, the word substitution for 1.10-style post-process shaders |
| `GlideVertexStream` | `glide_vertex_stream.{h,cpp}` | a streaming VBO and VAO in place of immediate mode; vertices are the floats immediate mode received |
| `GlideFramebufferPair` | `glide_framebuffer_pair.{h,cpp}` | front/back FBOs the picture rectangle's size with shared depth, exchange, readback, presentation to the window |
| `GlUnitQuad`, `BuildGlProgram` | `gl_unit_quad.{h,cpp}` | the unit quad for presentation and post-processing, programs built on the shared attribute slots |

The projection (`glOrtho`), the point size and the alpha test (`glAlphaFunc`) are uniforms and a `discard` in the combine shader. The game draws only into the offscreen pair, and `grBufferSwap` exchanges the two and draws the new front into the window. After a swap back is therefore the previous front (the page flip Task 729 relies on), and WebGL clearing the window's framebuffer after showing it, or showing a frame mid-draw, cannot touch the game's picture. After the present and the LFB blit the backend puts the game's state back from what it holds (`GameGlState`) without asking GL (GL queries stall a threaded driver, and halved the frame rate in measurement).

## Glide GLSL renderer boundary

`GlideLogicalState` preserves original Glide enums and initial raster state without platform dependencies. `GlideOpenGlBackend` owns the main-thread SDL3 window/OpenGL context and state translation, while the separate `GlideOpenGlShader` owns shader entry-point resolution, compilation/linking, the program, and combine uniforms. A synchronous backend queue executes guest-worker calls on the main thread.

The backend maps Glide comparison values `0..7` to OpenGL `GL_NEVER..GL_ALWAYS`. `grDepthBufferMode` owns depth-test enablement, while `grDepthBufferFunction` selects only the comparison. After the guest return address and catalog signature have both been validated, any specialized-handler failure uses a common decline path that supplies a conservative return and performs normal stdcall EIP/ESP cleanup. Invalid return addresses and signature mismatches remain hard rejects because their ABI is not trustworthy.

The platform-neutral `GlideImplementationIssueTracker` classifies unimplemented functions, unsupported arguments, backend failures, and ABI rejects while coalescing repeat counts by function/argument combination. Unimplemented and unsupported calls print immediate `[repiu-fatal]` lines and final `critical/FATAL` records, but verified stdcall ABIs still clean up normally and continue. It retains at most 128 unique records plus the first eight arguments, with separate per-category totals and overflow accounting.

On a window-scale change, an ordinary resize, or a fullscreen switch, Task 769's `ComputeGlideLetterboxRect` centres a content rect of the logical aspect ratio in the drawable, and the viewport, scissor and point size follow that rect; the leftover bars are cleared black just before each present. LFB readback samples only the content rect and nearest-neighbor downsamples it to the logical dimensions, preserving the original guest-visible LFB size and row ordering and keeping the bars out of what the guest reads. A double click or `Alt+Enter` switches between windowed mode and borderless fullscreen that never changes the display mode (`SDL_SetWindowFullscreen` with no mode set), and `Alt+1..4` is ignored while fullscreen. Since issue #45 the rectangle comes from `ComputeGlidePictureRect(..., keep_aspect)`: the letterbox rectangle above with keep-aspect on (the default, `REPIU_GLIDE_KEEP_ASPECT`), the whole drawable with it off. Every other consumer uses the same `content_rect_`, so `ApplyDrawableViewport` is the one place that changed. Fullscreen at start (`REPIU_GLIDE_FULLSCREEN`) is entered after the scaled window exists. A value changed through the OSD checkboxes, `Alt+Enter` or a double click reaches the loader through a session sink (`include/repiu/engine/session_display_preferences.h`), and the loader stores only `[Video] fullscreen` and `keep_aspect` in `cfg/repiu.ini`. Applying the stored value at start and SDL leaving fullscreen as the window closes are not reported. The engine knows no launcher types, and without a sink (probes, tools) nothing is written.

The SDL window title combines the CMake-validated `REPIU_VERSION` from the root `VERSION` file, the build identity label (platform/architecture and configuration such as `Win/x86 Debug` -- Task 738, `repiu::platform::BuildIdentityLabel`, with the configuration from CMake's `REPIU_BUILD_CONFIG="$<CONFIG>"`), the backend compilation date from `__DATE__`, the active `ExecutionBackend` name, and measured FPS. Execution orchestration passes the platform-neutral backend enum to `GlideOpenGlBackend`, which measures successful guest buffer swaps over roughly one-second monotonic-time periods. Title creation and updates both run on the executor main thread that owns the SDL window. `VERSION` is a configure dependency, so changing it automatically regenerates the build system.

Both `SDL_EVENT_QUIT` and `SDL_EVENT_WINDOW_CLOSE_REQUESTED` record a host exit request. The executor polling loop detects the request, stops the guest worker through the same context-recovery procedure used by timeout teardown, and then cleans up SDL/OpenGL and the translation worker on the main thread. The event handler never exits the process or destroys resources directly.

Glide gate live telemetry version 5 publishes ordinal, ESP, EBX/ECX/EDX, and eight stack dwords. This diagnostic boundary preserves caller evidence when an unimplemented gate terminates the child. Screen width/height return integer values in EAX as proven by the original caller; documented API type assumptions never override binary call-site evidence.

Glide2 state save/restore uses a fixed 312-byte platform-neutral image. Shared HLE code owns deterministic little-endian serialization and validation; guest ABI code only performs range-checked transfer and stdcall return. The image contains no host pointer. Unknown bytes are zero, and unencoded logical fields are preserved during the currently observed immediate opaque round-trip. Renderer backend replay is required before supporting Get/Set pairs with intervening state mutations.

Observed dither mode 2 is stored in `GlideLogicalState`, serialized in state-image version 2, and delegated to host `GL_DITHER`. This is a compatibility stage, not a pixel-fidelity claim. A future replaceable shader policy may implement verified Voodoo ordered dithering without changing guest ABI integration.

### 원근 텍스처와 table fog 계약 (Task 359)

PIU의 확인된 60-byte `GrVertex` producer에서 dword 8은 공용 `oow`, dword 9/10은
TMU0 `sow/tow`입니다. boundary는 이 값을 `GlideDrawVertex`에 분리해 전달하고,
backend는 `sow/tow`만 Glide coordinate extent로 정규화합니다. vertex shader가
분자와 공용 `oow`를 선형 보간한 뒤 fragment shader가 UV를 `oow`로 나누므로,
직교 host projection을 유지하면서 원본 Glide의 perspective correction을 보존합니다.
dword 11..14는 PIU 표본에서 아직 유효한 per-TMU reciprocal-w로 확인되지 않았으므로
projected-texture 증거가 생기기 전에는 사용하지 않습니다.

`GlideLogicalState`는 `grFogTable`의 64바이트를 즉시 복사하며, 플랫폼 공용
`glide_fog`가 공식 `W(i)` knot와 선형 보간을 정의합니다. Win32 shader는 관측된
`GR_FOG_DISABLE`과 `GR_FOG_WITH_TABLE`만 지원하고, fog color와 table을 uniform으로
소유합니다. LFB 표시는 같은 shader의 전용 blit mode로 geometry combine/fog를
bypass하고 depth, blend, cull, alpha test, scissor, color mask, draw buffer, texture
binding을 일시 격리합니다.

### Perspective texture and table-fog contract (Task 359)

In PIU's confirmed 60-byte `GrVertex` producer, dword 8 is shared `oow` and
dwords 9/10 are TMU0 `sow/tow`. The boundary carries them separately in
`GlideDrawVertex`, and the backend normalizes only `sow/tow` by the Glide
coordinate extent. The vertex shader linearly interpolates the numerators and
shared `oow`; the fragment shader divides UV by `oow`, preserving Glide
perspective correction under the host's orthographic projection. Dwords 11--14
remain unused until producer evidence confirms a projected-texture layout.

`GlideLogicalState` immediately copies the 64 bytes passed to `grFogTable`, and
platform-neutral `glide_fog` defines the documented `W(i)` knots and linear
interpolation. The Win32 shader supports the observed `GR_FOG_DISABLE` and
`GR_FOG_WITH_TABLE` modes and owns fog color/table uniforms. LFB presentation
uses a dedicated shader blit mode that bypasses geometry combine/fog while
temporarily isolating depth, blend, cull, alpha test, scissor, color mask, draw
buffer, and texture binding.

### 선 primitive와 cull 변환 (Task 374)

플랫폼 공용 `glide_vertex`가 관측된 60바이트 producer image를
`GlideDrawVertex`로 decode합니다. Win32 boundary는 guest 범위를 검증하고 로컬 image로
복사한 뒤 decoder를 호출합니다. `grDrawLine`과 `grDrawTriangle`은 backend의 같은
primitive 제출 경로를 사용하므로 색, texture extent, perspective, fog 입력이 같습니다.
line은 `GL_LINES`와 폭 1로 제출합니다.

`TranslateGlideOpenGlCullMode`는 Glide mode와 window origin을 함께 받아 OpenGL
front/back face로 변환합니다. lower-left에서 negative/positive는 back/front이고,
upper-left에서는 반대입니다. mode 0은 face culling을 끕니다.

### Line primitives and cull translation (Task 374)

Platform-neutral `glide_vertex` decodes the observed 60-byte producer image into
`GlideDrawVertex`. The Win32 boundary validates and copies guest memory before
decoding. `grDrawLine` and `grDrawTriangle` share backend primitive submission,
so color, texture extent, perspective, and fog inputs stay identical; lines use
one-pixel `GL_LINES`.

`TranslateGlideOpenGlCullMode` combines the Glide mode with the window origin.
Lower-left negative/positive map to OpenGL back/front, upper-left reverses the
mapping, and mode zero disables face culling.

`_GRTEXTEXTUREMEMREQUIRED@8`는 guest `GrTexInfo`를 읽는 플랫폼 공용 계산으로 모델링하며 검증된 8-byte 정렬 texture 크기를 EAX로 반환합니다. 관측된 texture download/source/sampler/combine 호출은 OpenGL texture cache와 GLSL sampler/combine으로 연결됩니다. 미관측 포맷과 projected-texture layout은 검증 증거 없이 지원으로 확장하지 않습니다.

`_GRTEXTEXTUREMEMREQUIRED@8` is modeled as a platform-neutral calculation over
guest `GrTexInfo`, returning a validated eight-byte-aligned texture size in
EAX. Observed texture download/source/sampler/combine calls feed the OpenGL
texture cache and GLSL sampler/combine path. Unobserved formats and the
projected-texture layout are not claimed without validation evidence.

P_8/AP_88 texture의 원본 index texel과 palette 수명은 분리합니다. Win32 backend는
palette byte가 실제로 바뀔 때만 generation을 증가시키며, 모든 보존 texture를 즉시
재업로드하지 않습니다. textured draw의 공용 준비 단계는 현재 texture의 적용
generation을 확인하고 오래된 경우에만 보존 source를 RGBA로 디코드하여 기존 OpenGL
allocation에 `glTexSubImage2D`로 갱신합니다. 따라서 palette 변경은 다음 sampling 전에
반영되지만 화면에 쓰이지 않는 texture는 비용을 만들지 않습니다. texture census는 palette
download의 동일/변경 분류와 지연 갱신 수·실패·바이트·decode/upload host 시간을 보고합니다.

Original index texels and palette lifetimes remain separate for P_8/AP_88.
The Win32 backend advances a generation only when palette bytes actually change
and no longer re-uploads every retained texture immediately. Shared textured-draw
preparation checks the current texture's applied generation and, only when stale,
decodes its retained source to RGBA and updates the existing OpenGL allocation with
`glTexSubImage2D`. A palette change is therefore visible before the next sample,
while unused textures incur no work. The texture census reports identical versus
changed palette downloads and lazy-refresh counts, failures, bytes, and host-side
decode/upload time.

## Win32 로더 앱 배치

현재 실제 Win32 로더 executable target은 `repiu`이다.

진입점은 `src/host/loader/main.cpp`에 두며, `src/tools/` 아래의 분석 도구와 구분한다. Task 731 이전에는
`src/host/win32/main.cpp`였으나, Windows 헤더를 쓰지 않는 플랫폼 공용 코드이므로 역할 이름의 위치로 옮겼다.
로그 줄의 `"Win32 ..."` 접두어는 Task 732에서 제거했다. 모든 줄이 이미 `[loader]` 로거 태그를 달고 있으므로
`[loader] minimal execution timed out: true`처럼 읽힌다. 스크립트와 가이드도 같은 작업에서 갱신했고,
`tests/history/`와 작업 로그·분석 문서의 과거 인용은 당시 출력의 기록이므로 바꾸지 않았다.

이 진입점은 현재 target profile 선택, 원본 executable 읽기, DOS/4GW load result 생성, relocated runtime image plan 생성, relocated image buffer 생성, Win32 process memory 배치, minimal execution trampoline 호출을 순서대로 담당한다.

`src/tools/exe_analyzer/`는 계속 비실행 분석 도구로 남긴다.

## Win32 Loader App Layout

The current practical Win32 loader executable target is `repiu`.

Its entry point lives in `src/host/loader/main.cpp`, separate from analysis tools under `src/tools/`. Before
Task 731 it was `src/host/win32/main.cpp`; being platform-neutral code that includes no Windows header, it moved
to a location named by role. Task 732 dropped the `"Win32 ..."` prefix from its log lines: every line already
carries the `[loader]` logger tag, so they read `[loader] minimal execution timed out: true`. Scripts and guides
were updated in the same task; quotations in `tests/history/`, work logs and analysis documents record what the
output said at the time and were left unchanged.

This entry point currently owns target profile selection, original executable reading, DOS/4GW load result creation, relocated runtime image planning, relocated image buffer creation, Win32 process-memory placement, and minimal execution trampoline invocation.

`src/tools/exe_analyzer/` remains a non-executing analysis tool.

## Win32 execution_trampoline 분해 / Win32 execution_trampoline decomposition

`src/engine/execution_trampoline.cpp`는 한때 12,117줄 단일 파일이었으나, Task 233(Phase 1)에서 동작 보존 리팩토링으로 서브시스템별 모듈로 분해했다(약 3,200줄로 축소). 트램폴린에는 VEH 디스패치 코어, 실행 드라이버(`RunWin32ExecutionThread`/`AttemptWin32*`), 여러 모듈이 공유하는 substrate만 남겼다. 모듈들은 `src/engine/` 아래 서브디렉토리로 그룹화되며(Task 520 이전에는 `src/platform/win32/`), 짧은 이름 include는 CMake include 경로로 해석된다.

Originally a single 12,117-line file, `execution_trampoline.cpp` was decomposed in Task 233 (Phase 1) via behavior-preserving refactoring into per-subsystem modules (down to ~3,200 lines). The trampoline retains only the VEH dispatch core, the execution driver, and the substrate shared across modules. Modules are grouped into subdirectories under `src/engine/` (`src/platform/win32/` before Task 520); short-name includes resolve via CMake include directories.

| 디렉토리 / directory | 모듈 / module | 책임 / responsibility |
|---|---|---|
| `execution/` | `execution_trampoline.cpp`, `thread_context.h`, `execution_internal.h`, `win32_thread_api.h` | VEH 디스패치·실행 드라이버·공유 상태(ThreadContext)·경계 선언·kernel32 스레드 API |
| `exception/` | `exception_rescue_win32` | VEH 엔트리, `ExceptionDispatchScope`, 복구 전역 |
| `io/` | `port_io_emulator` | IN/OUT 포트 I/O 트랩 |
| `dos/` | `dos_int21_services`, `dpmi_mscdex_services` | DOS INT 21h/2Fh, DPMI INT 31h, 마우스 INT 33h, MSCDEX |
| `cpu_emul/` | `instruction_emulation`, `guest_memory_access` | 레지스터/플래그/디코드·세그먼트·traced 메모리·REP 명령 에뮬, 게스트/섀도 메모리 접근 |
| `aot/` | `aot_runtime_dispatch`, `aot_ff_boundary_attribution`, `aot_ff_boundary_target_attribution` | AOT 번역 워커·전이/재진입 디스패치·코드쓰기 watch·bounded `FF /4` site/target 관측 |
| `boundary/` | `linexe_glide_boundary` | linexe far-transfer·Glide 게이트·allocator 제어흐름 |
| `telemetry/` | `live_telemetry_snapshot`, `guest_address_watch`, `fault_exit_trace`, `fault_recovery_provenance`, `linux_x64_transfer_failure_provenance` | 라이브 텔레메트리 매핑·실행 스냅샷, guest 주소 하나의 실행 경로 관측(`REPIU_GUEST_WATCH`, Task 581), 거절된 폴트의 exit site·복구 전 원인·unresolved Linux x64 transfer producer 출력(`REPIU_FAULT_EXIT_TRACE`, Tasks 582, 647, 648) |

```mermaid
flowchart TD
    ET["execution/ (트램폴린: VEH 디스패치 + 드라이버 + substrate)"]
    ET --> EX["exception/"]
    ET --> IO["io/"]
    ET --> DOS["dos/"]
    ET --> CPU["cpu_emul/"]
    ET --> AOT["aot/"]
    ET --> BND["boundary/"]
    ET --> TEL["telemetry/"]
    DOS -. "traced 인터럽트 래퍼" .-> CPU
    CPU --> CPU
    AOT -. "메모리/디코드 재사용" .-> CPU
```

크로스-TU 경계 함수(예: `WriteGuestBytes`, `IsAotCacheAddress`, `RecordHandledDosInterrupt`, `ResolveSegmentLinearRange`)는 익명 네임스페이스 밖으로 승격해 외부 링크로 만들고 `execution_internal.h`에 선언한다. Phase 2(중립 `GuestCpuFrame` seam)·3(서비스 의미론의 플랫폼 중립화)은 두 번째 플랫폼 백엔드가 필요해질 때 착수한다.

Cross-TU boundary functions (e.g., `WriteGuestBytes`, `IsAotCacheAddress`, `RecordHandledDosInterrupt`, `ResolveSegmentLinearRange`) are promoted out of the anonymous namespace to external linkage and declared in `execution_internal.h`. Phase 2 (a neutral `GuestCpuFrame` seam) and Phase 3 (making service semantics platform-neutral) are deferred until a second platform backend is needed.

## Win32 Loader Log Level Policy

Win32 loader logs use levels to separate normal progress from the current implementation blocker.

The loader log pattern is `[%X.%e] [%8l] [%n] %v`.
This prints millisecond timestamps, fixed-width level names, logger name, and message text.

`info` is used for normal loader progress, selected runtime addresses, successful placement, handled HLE/DOS/segment counts, and normal guest output.

`warn` is used for expected host-environment constraints that the loader can work around, such as fixed low-address range probe or reservation failure followed by relocated execution.

`error` is used for loader-stage failures and for the current original-code execution blocker. When original entry execution stops with a caught SEH exception, the exception registers, relocated byte window, unknown instruction classification, and current blocker message are printed as `error` while preserving the existing observation-oriented process exit behavior.
# DOS4GW object selector allocation / DOS4GW object selector 할당

PIU의 외부 DOS4GW `LINEXE.EXP` loader는 LE object마다 DPMI descriptor를 동적으로 하나씩 할당하고 base, limit, access rights를 설정한다. Runtime image materialization은 이 순서를 `SelectorAllocator`로 모델링하고, 할당 결과를 16:16 far-pointer fixup 및 실행 `SelectorTable`에 동일하게 사용한다. PIU 프로필의 첫 object selector `0x1C`는 관찰된 object 2=`0x24`, object 3=`0x2C`에서 역산한 초기 DPMI 상태이며 LE object 번호의 고정 공식이 아니다.

The external DOS4GW `LINEXE.EXP` loader dynamically allocates one DPMI descriptor per LE object, then sets its base, limit, and access rights. Runtime image materialization models this order with `SelectorAllocator` and uses each allocation result for both 16:16 far-pointer fixups and the execution `SelectorTable`. The PIU profile's first object selector, `0x1C`, is inferred from the observed object 2=`0x24` and object 3=`0x2C` values; it is initial DPMI state, not a fixed formula derived from the object index.

## Live execution telemetry / 실시간 실행 telemetry

Single-step PIU 실행은 guest/host 공유 atomic heartbeat를 사용한다. host poll은 시작 시점과 1초 간격으로 stderr snapshot을 출력할 수 있다. Task 490부터 wall-clock 예산과 무진행 감시는 독립적이다. `REPIU_EXECUTION_TIMEOUT_MS`는 전체 실행시간만 제한하고, `REPIU_STALL_TIMEOUT_MS`는 diagnostic, single-step, AOT boundary/reentry와 Glide direct-dispatch 진입이 모두 변하지 않은 시간을 제한한다. 두 설정 모두 `0`이 기본값이며 비활성이다. 실행 결과는 wall timeout과 stall timeout을 구분하고, timeout observation은 guest thread를 종료하고 join한 뒤 복사하여 guest가 수정 중인 비원자 container와의 data race를 방지한다.

Single-step PIU execution uses atomic heartbeat state shared by the guest and host. The host poll can emit stderr snapshots at startup and once per second. Since Task 490, the wall-clock budget and no-progress watchdog are independent. `REPIU_EXECUTION_TIMEOUT_MS` limits total execution time, while `REPIU_STALL_TIMEOUT_MS` limits the interval during which diagnostic, single-step, AOT boundary/reentry, and Glide direct-dispatch entry all remain unchanged. Both settings default to `0`, disabled. The result distinguishes wall and stall timeouts, and timeout observations are copied only after terminating and joining the guest, preventing races with non-atomic containers still being modified by the guest.

Task 705부터 host-thread interrupt callback은 native context write-back 필요 여부를
반환합니다. 순수 sampler와 guest/AOT 밖에서 복귀를 거절한 shutdown callback은 false를
반환하여 32-bit `GuestCpuContext`를 native register에 다시 쓰지 않습니다. 실제 recovery처럼
register를 편집한 callback만 true를 반환합니다. 이는 Linux x64 host code의 64-bit R15
값이 guest ESP용 R15D zero-extension에 의해 잘리는 것을 막으면서, signal 전달과 응답
성공 여부는 기존대로 유지합니다.

Starting with Task 705, host-thread interrupt callbacks report whether native
context write-back is required. Pure samplers and shutdown callbacks that refuse
recovery outside guest/AOT code return false, so the 32-bit `GuestCpuContext` is
not stored back into native registers. Only callbacks that edit registers, such
as successful recovery, return true. This prevents a Linux x64 host-code R15
pointer from being truncated by guest-ESP R15D zero-extension while preserving
the existing signal-delivery and answered-interrupt contract.

Task 721부터 Linux x64의 wall-clock guest-position census는
`REPIU_LINUX_X64_NATIVE_SAMPLE=1`일 때만 target-thread interrupt를 사용합니다. 이
callback은 guest ABI의 32-bit register snapshot만 복사하고 false를 반환하므로 native
context는 write-back하지 않습니다. Win32/i386의 host stack scan은 Linux signal handler에서
제외됩니다. Linux x64 host RIP는 guest ABI 폭을 넘으므로 현재 census의 host 항목은 위치
분포의 보조 증거일 뿐 symbolizable native call site가 아닙니다.

Starting with Task 721, the Linux x64 wall-clock guest-position census uses a
target-thread interrupt only when `REPIU_LINUX_X64_NATIVE_SAMPLE=1`. Its callback
copies only the guest ABI's 32-bit register snapshot and returns false, so native
context is never written back. The Win32/i386 host stack scan is excluded from the
Linux signal handler. Since a Linux x64 host RIP exceeds the guest ABI width, the
current census host entries are supporting distribution evidence, not symbolizable
native call sites.

Task 722부터 Linux x64 opt-in census는 signal `ucontext_t`의 full native RIP를
`NativePhaseSample::native_instruction_pointer`에 별도로 보존합니다. 이는 고정 32-bit
guest ABI를 바꾸지 않으며 read-only callback의 no-write-back 계약도 유지합니다.
`REPIU_LINUX_X64_NATIVE_SAMPLE_TRACE=1`일 때만 poll thread가 capture 뒤에 경과 시간,
full RIP, guest EIP 및 cache mapping 여부를 한 줄로 기록합니다. formatting과 I/O는 signal
handler 밖에서만 수행하므로 trace는 진단용이며 기본 실행 및 Win32 x86 동작은 바꾸지 않습니다.

Starting with Task 722, the Linux x64 opt-in census retains the signal `ucontext_t`'s
full native RIP separately in `NativePhaseSample::native_instruction_pointer`. This
does not alter the fixed 32-bit guest ABI and preserves the read-only callback's
no-write-back contract. Only with `REPIU_LINUX_X64_NATIVE_SAMPLE_TRACE=1` does the
poll thread write one post-capture line with elapsed time, full RIP, guest EIP, and
cache-mapping status. Formatting and I/O remain outside the signal handler, so the
trace is diagnostic only and leaves default execution and Win32 x86 unchanged.

Task 724부터 `grLfbLock`의 staging seed는 `GlideLfbTimingProfile`로 readback과
RGBA8→565 encode를 분리 관찰할 수 있습니다. `REPIU_GLIDE_LFB_TIME_PROFILE=1|on|true`일
때만 gate handler가 cycle counter를 읽으며, profile은 OpenGL 호출 순서·guest memory·lock
반환값을 바꾸지 않습니다. 종료 뒤 `MinimalExecutionAttempt` snapshot과 final report가
성공·실패 수, 각 단계 누적/최대 cycles 및 역행 counter clamp 수를 보고합니다.

Starting with Task 724, `GlideLfbTimingProfile` can separately observe readback and
RGBA8-to-565 encoding in the `grLfbLock` staging seed. Only with
`REPIU_GLIDE_LFB_TIME_PROFILE=1|on|true` does the gate handler read cycle counters;
the profile does not alter OpenGL call order, guest memory, or lock return values.
After shutdown, the `MinimalExecutionAttempt` snapshot and final report expose success
and failure counts, phase aggregate/maximum cycles, and backward-counter clamps.

Task 725는 별도 opt-in `GlideLfbWriteFootprintProfile`을 추가합니다.
`REPIU_GLIDE_LFB_WRITE_CENSUS=1|on|true`일 때 성공한 write lock은 seeded RGB565 staging
bytes를 private host storage에 복사하고, 대응 unlock은 기존 decode/present 전에 pixel을
비교합니다. report는 unchanged, partial-extent, full-extent, every-pixel-changed lock과
누적/최대 changed·bounding-box pixel을 구분합니다. 이는 byte-difference 관찰일 뿐이므로
guest가 같은 값을 다시 쓴 경우는 보이지 않으며, 이 결과만으로 필수 readback을 생략할 수 없습니다.

Task 725 adds a separate, opt-in `GlideLfbWriteFootprintProfile`. With
`REPIU_GLIDE_LFB_WRITE_CENSUS=1|on|true`, a successful write lock copies its seeded
RGB565 staging bytes into private host storage and its matching unlock compares pixels
before the existing decode/present path. The report distinguishes unchanged, partial-
extent, full-extent, and every-pixel-changed locks, plus aggregate and maximum changed/
bounding-box pixels. This is byte-difference evidence only: an identical guest rewrite
is invisible, so it cannot by itself justify skipping the required readback.

Linux x64 native-write telemetry는 Linux SysV x64 dispatch frame ABI에 종속되므로
공용 `repiu_exe` source 목록이 아니라 `UNIX AND NOT EMSCRIPTEN`이며 host pointer가
64-bit인 target에만 편성합니다. Win32 x86은 이 translation unit과 Linux x64 frame
header를 컴파일하지 않으며, Linux x64 ABI의 pointer-width 및 layout assertion은 그대로
유지합니다.

Linux x64 native-write telemetry depends on the Linux SysV x64 dispatch-frame
ABI, so it is compiled only for `UNIX AND NOT EMSCRIPTEN` targets with 64-bit
host pointers rather than being part of the common `repiu_exe` source list.
Win32 x86 therefore does not compile this translation unit or its Linux x64
frame header, while the Linux x64 ABI pointer-width and layout assertions remain
intact.

Child 내부 telemetry를 회수할 수 없는 경우 `repiu_supervisor_win32`가 named shared memory를 생성하고 mapping 이름을 환경 변수로 전달한다. loader의 host/guest는 고정 버전 POD에 interlocked write하고 supervisor는 child 출력과 독립적으로 snapshot을 읽고 deadline에 child를 terminate/join한다.

When child-local telemetry cannot be recovered, `repiu_supervisor_win32` creates named shared memory and passes its mapping name through the environment. Loader host/guest paths use interlocked writes into a versioned POD, while the supervisor reads snapshots independently of child output and terminates/joins the child at a deadline.

## Contiguous allocator arena / 연속 allocator arena

PIU 프로필은 LE image reserve 뒤에 16 MiB의 실제 contiguous expansion을 둔다. 현재 Win32 placement는 전체 `0x015D7000` 범위를 reserve/commit하여 원본 guest load/store가 arena 확장 객체를 직접 접근하게 한다. shadow memory는 실제 arena 밖의 진단 안전망이며 정상 allocator backing을 대신하지 않는다.

The PIU profile provides 16 MiB of real contiguous expansion after the LE image reserve. Current Win32 placement reserves and commits the full `0x015D7000` range so original guest loads/stores directly access expanded allocator objects. Shadow memory remains a diagnostic safety net outside the real arena and does not replace normal allocator backing.

## Shadow segment synchronization / Shadow segment 동기화

Single-step HLE가 처리한 segment load는 `ThreadContext` shadow selector를 변경한다. 원본 코드가 stack으로 selector를 복원하는 관찰된 32비트 `POP DS`도 HLE가 `[ESP]`의 selector를 읽고 shadow DS 및 ESP/EIP를 함께 갱신한다. access-violation HLE 뒤에도 후속 segment 복원을 관찰할 수 있도록 single-step 모드에서는 guest exception dispatch가 TF를 보존한다.

Segment loads handled by single-step HLE update the shadow selector in `ThreadContext`. For the observed 32-bit POP DS restoration, HLE reads the selector from `[ESP]` and updates shadow DS, ESP, and EIP together. Guest exception dispatch preserves TF in single-step mode so later segment restoration remains observable after access-violation HLE.

연속된 지원 segment load는 한 single-step dispatch에서 함께 처리하여 첫 명령을 건너뛴 뒤 두 번째 명령이 shadow 갱신 없이 native 실행되는 것을 막는다. 관찰된 EAX=0/DF=0 `REP STOSD`는 전체 destination span이 guest arena 안일 때 일괄 zero-fill한다.

Adjacent supported segment loads are consumed in one single-step dispatch so the second instruction cannot execute natively without a shadow update after the first is skipped. The observed EAX-zero/DF-clear REP STOSD is batched only when its entire destination span lies inside the guest arena.

관찰된 register-direct `MOV r16,Sreg`는 Win32 실제 segment selector가 guest로 누출되지 않도록 shadow selector를 목적 register 하위 16비트에 기록한다.

Observed register-direct MOV r16,Sreg instructions write the shadow selector into the destination register's low 16 bits so native Win32 segment selectors cannot leak into guest state.

Descriptor-backed segment byte reads translate selector+offset through `SelectorTable`, use DOS low-memory backing below 64 KiB, and otherwise read only validated guest arena memory. Observed ES-prefixed byte MOV and CMP forms use shadow registers and x86 byte arithmetic flags.

## 원본 fatal tail 실행 / Original fatal-tail execution

guest breakpoint는 기본적으로 중단한다. 단, 원본 image 내부에서 `CC 52 E8 rel32 F4` fatal-tail signature가 확인되면 breakpoint 주소와 `EDX` ASCIZ message를 기록한 뒤 원본 `push edx; call error-printer`로 재개한다. Linux x64 AOT 실행에서는 처리된 breakpoint 다음 주소를 명시적인 guest boundary로 분류해 기존 cache entry 또는 동적 translation으로 재진입하며, 재진입할 수 없는 long-mode 비동일 명령은 raw guest code로 실행하지 않는다. 이 경로에서 관찰된 DOS `AH=09h`, low-memory register-frame `REP MOVS`, 제한된 DPMI `AX=0300h/BL=2Fh`를 HLE하고, 원본 DOS terminate를 우선한다.

Guest breakpoints stop by default. Only a confirmed `CC 52 E8 rel32 F4` fatal-tail signature inside the original image records the breakpoint and bounded `EDX` ASCIZ message, then resumes the original `push edx; call error-printer`. Under Linux x64 AOT execution, the address after the handled breakpoint is classified as an explicit guest boundary and re-enters an existing cache entry or dynamic translation; a long-mode-non-identical instruction that cannot re-enter is never executed as raw guest code. The observed DOS `AH=09h`, low-memory register-frame `REP MOVS`, and narrowly scoped DPMI `AX=0300h/BL=2Fh` path are handled while preserving the original DOS termination path.

## Win32 VEH and host recovery boundary

The guest worker owns one process-global active VEH context because guest execution is serialized per loader process. The parent removes the VEH only after joining the worker. Guest-stack recovery records the entry-time segment selectors and clears TF/DF; reliable DS/FS restoration before returning to compiler-generated C++ remains the current recovery frontier.

Host recovery now saves entry-time selectors in serialized global recovery slots and reads them with a `CS:` override before returning to C++. Residual single-step exceptions at host addresses clear TF and continue without nested guest recovery. The executor main thread creates, services, and destroys SDL3/OpenGL resources; the guest worker blocks on synchronous rendering commands. The parent removes the VEH after joining the worker. DOS stdout and stderr are accumulated separately and emitted through an executable-name spdlog logger at info and error levels.

DOS file diagnostics include a bounded 64-entry read/seek ring. It preserves chronological handle, path, position, size, result, guest EIP/ESP, eight stack dwords, and bounded read-prefix evidence without changing guest-visible file behavior. The protected-mode DOS/4GW `INT 21h AH=3Fh` bridge consumes the 32-bit byte count in `ECX` and returns the 32-bit byte count in `EAX`; reducing these values to real-mode `CX/AX` breaks large Watcom reads.

DOS 문자 디바이스는 host regular-file VFS와 분리합니다. 현재 관찰된
`CON`/`CON:` 경로는 DOS 경로의 마지막 구성요소에서 대소문자 구분 없이
인식되고, 일반 user handle을 받으며 host path를 갖지 않습니다.
protected-mode `INT 21h AH=40h` bridge는 `CON` handle을 공용 DOS console
output sink로 전달하고 요청된 byte 수를 반환합니다. read, keyboard 의미론,
관찰되지 않은 DOS device는 binary evidence가 생길 때까지 이 경계 밖에
둡니다.

DOS character devices remain separate from the host regular-file VFS. The
currently observed `CON`/`CON:` path is recognized case-insensitively in the
final DOS path component, receives a normal user handle, and has no host path.
The protected-mode `INT 21h AH=40h` bridge routes a `CON` handle to the shared
DOS console output sink and returns the requested byte count. Reads, keyboard
semantics, and unobserved DOS devices remain outside this boundary until binary
evidence requires them.

콘솔 문자 입력은 Task 764부터 `INT 21h AH=07h/08h`(에코 없는 한 문자)로
제공합니다. DOS의 콘솔 입력은 BIOS 키보드 위에 서 있으므로 이 서비스는 INT 16h
`AH=00h`와 같은 버퍼(`ThreadContext::bios_keyboard`)를 legacy 형식으로 읽어 `AL`에
문자를 돌려주고, 확장 키는 scan code를 다음 호출에 돌려줍니다. 버퍼가 비면 1 ms 잔
뒤 `EIP`를 올리지 않고 돌아와 게스트가 같은 `int 21h`를 다시 실행합니다 — DOS의
"키가 올 때까지 기다림"을 HLE 안의 재시도로 옮긴 것이며, 키는 호스트 스레드의 폴
루프가 계속 채웁니다. Ctrl-C 검사(INT 23h)는 하지 않습니다. 게임의 fatal 처리기가
`getch`로 쓰는 경로가 이 서비스에 닿습니다.

Console character input is provided since Task 764 as `INT 21h AH=07h/08h` (one
character without echo). DOS console input stands on the BIOS keyboard, so the
service reads the same buffer INT 16h `AH=00h` does (`ThreadContext::bios_keyboard`)
in its legacy form, returns the character in `AL`, and hands an extended key's scan
code out on the next call. With the buffer empty it sleeps 1 ms and returns without
advancing `EIP`, so the guest re-executes the same `int 21h`: DOS's "wait for a key"
turned into a retry inside the HLE, with the host thread's poll loop still feeding
keys. There is no Ctrl-C check (INT 23h). The game's fatal handler reaches this
service through `getch`.

Win32 native execution uses a fail-closed function return fast path implemented by `native_fast_path.*` and `verified_region_analyzer.*`. Pinned Zydis v4.1.1 decodes observed direct-call targets in legacy 32-bit mode; rePIU recursively verifies runtime-bounded direct control flow and rejects privileged, interrupt, I/O, system, segment-dependent, indirect, far, or undecodable paths. An approved function runs with Trap Flag cleared until an x86 hardware execution breakpoint at its validated guest return address reenters VEH. Any intermediate exception restores debug registers and single-step state and permanently rejects that function for the current run.

Task 275의 `native_linear_span.*`은 함수 진입으로 증명되지 않은 일반 single-step
지점의 coverage를 보완합니다. Zydis가 다음 민감 명령, 제어 전이, 명시적 memory write를
경계로 찾고 그 앞에 일반 명령이 두 개 이상이면 Dr0 실행 breakpoint를 경계에 설치한 뒤
TF를 끕니다. 경계 #DB는 debug register와 TF를 복원하고 기존 single-step/HLE chain에
경계 명령을 넘깁니다. memory write는 span 밖에서 실행되며 scan 결과를 cache하지 않아
self-modifying code 뒤의 stale decode를 재사용하지 않습니다. 예상하지 않은 exception도
같은 fail-closed 복원 경로를 사용합니다.

Task 288은 이 위에 세 가지 옵트인 실험(entry별 scan 캐시, memory-write 통과, 전방
`jmp` 연결)을 더했지만 모두 기본 OFF로 남았고(hit 0, draw/swap 약 20% 감소, forward
chain 0회), issue #22에서 스위치와 함께 삭제했습니다. 다만 span 안의 `push` 같은 암묵적
스택 write가 write-watch page에서 fault를 내는 경우는 기본 span에서도 생기므로, 그
fault를 일반 cancel과 따로 세는 `write fault-cancel` 집계는 남아 있습니다.

Task 287의 반복 A/B 뒤 `dynamic`는 환경 변수 미지정 시 이 span을 기본 활성화합니다.
다른 backend의 기본값은 계속 OFF입니다. `REPIU_NATIVE_LINEAR_SPAN=1|on|true`는
backend와 무관하게 ON, `0|off|false`는 OFF이며 알 수 없는 값도 fail-closed OFF입니다.
프로그램 전체의 기본 실행 backend는 이 정책으로 바뀌지 않습니다.

The Task 275 `native_linear_span.*` path extends coverage from ordinary single-step
sites that cannot enter a verified function. Zydis finds the next sensitive instruction,
control transfer, or explicit memory write; when at least two ordinary instructions precede
it, Dr0 guards that boundary while TF is clear. The boundary #DB restores debug state and
TF, then passes the boundary instruction to the existing single-step/HLE chain. Memory
writes stay outside spans and results are not cached, preventing stale decoded spans after
self-modification. Unexpected exceptions use the same fail-closed restoration.

Task 288 added three opt-in experiments on top (a per-entry scan cache, crossing memory
writes, chaining forward `jmp`s); all stayed default off (zero hits, about 20% fewer
draws/swaps, zero forward chains) and issue #22 deleted them with their switches. A default
span can still fault on a write-watched page through an implicit stack write such as `push`,
so the `write fault-cancel` count, kept apart from ordinary cancellation, remains.

After Task 287's repeated A/B, `dynamic` enables spans by default when the environment is
unset; other backends remain off by default. `REPIU_NATIVE_LINEAR_SPAN=1|on|true` enables
the path for any backend, `0|off|false` disables it, and unknown values fail closed to
disabled. This policy does not change the program-wide default execution backend.
## Reentrancy-safe guest bulk copy

Win32 VEH instruction handling must not directly dereference a guest range when the access can recursively enter the handler. `REP MOVS` reads through a temporary buffer with `ReadProcessMemory` and writes through the guest-write helper, which temporarily applies writable page protection and restores it afterward.

```mermaid
flowchart LR
    VEH[VEH REP MOVS] --> RPM[ReadProcessMemory]
    RPM --> TEMP[temporary buffer]
    TEMP --> GW[guest write helper]
    GW --> RESUME[guest resumes]
```
## Host exceptions in the guest VEH

The Win32 guest VEH owns exceptions only when they belong to guest execution or an explicit HLE boundary. Host `DBG_PRINTEXCEPTION_C/W` events are consumed without guest recovery, while the Visual C++ thread-name exception is passed onward. Optional supervisor `debug-exceptions` mode observes first-chance events externally and preserves worker VEH ownership.

```mermaid
flowchart TD
    E[Win32 exception] --> G{Guest EIP?}
    G -->|yes| V[guest VEH/HLE]
    G -->|no| D{debug-print?}
    D -->|yes| C[continue execution]
    D -->|no| S[continue search]
```
## Supervisor-owned execution deadline

Supervised Win32 execution disables the loader's wall-clock timeout with `REPIU_EXECUTION_TIMEOUT_MS=0` and leaves the independently disabled stall timeout at `REPIU_STALL_TIMEOUT_MS=0`. The supervisor owns the deadline and terminates the complete child process, preventing `TerminateThread` from racing active VEH or host-call state.

```mermaid
flowchart LR
    S[supervisor deadline] --> P[complete child process]
    L[loader timeout disabled] --> G[guest remains active]
    G --> P
```
## Synchronous system timer tick HLE / 동기식 시스템 타이머 틱 HLE

BDA 선형 주소 `0x46C`의 BIOS tick과 프로그래밍 가능한 IRQ0 주기는 분리합니다.
플랫폼 공용 `hle::PitChannel0`은 포트 `0x43`/`0x40`의 채널 0 제어어와 reload
바이트를 보존하고, 원자적 generation/divisor snapshot을 게시합니다.
`PollThreadUntilExit`의 `hle::PitIrqSchedule`은 단조 증가 시계와
`1,193,280 / divisor` 비율로 IRQ0 만료를 계산합니다. `PIU.EXE`가 기록하는
divisor `4,972`는 정확히 `240Hz`입니다. BDA tick은 기본 divisor `65,536`으로
별도 계산합니다.

The BIOS tick at BDA linear address `0x46C` is separate from programmable
IRQ0 cadence. Platform-neutral `hle::PitChannel0` preserves channel-0 control
and reload bytes written to ports `0x43`/`0x40`, publishing an atomic
generation/divisor snapshot. `hle::PitIrqSchedule` in `PollThreadUntilExit`
derives expirations from monotonic time and `1,193,280 / divisor`. The
`PIU.EXE` divisor `4,972` is exactly `240Hz`; BDA time remains separately
derived from default divisor `65,536`.

```mermaid
flowchart LR
    O["OUT 43h/40h"] --> P["PitChannel0 snapshot"]
    P --> S["PitIrqSchedule"]
    S --> I["coalesced pending IRQ0"]
    D["default divisor 65536"] --> W["BDA 0x46C"]
```
## MAME CHD asset mount

PIU ROM-set target은 asset container 해석과 guest 실행을 분리합니다. `TargetProfile::rom_set_id`가 게임 ID 분기 없이 공용 ZIP/CHD mount를 선택합니다. 공용 ISO9660 reader는 data track이 여러 개인 멀티세션 CHD에서 마지막 data track을 선택하고, CHT2/CHTR track type에 따라 cooked `MODE1`/`MODE2_FORM1` offset 0, `MODE1_RAW` offset 16, `MODE2_RAW` offset 24를 적용합니다. 루트 자기 참조 레코드에서 signed extent-LBA bias를 찾아 single-session과 multisession data track을 같은 경로로 처리하며, 선택한 track의 PVD와 extent 범위 및 미지원 sector 형식은 fail-closed로 검증합니다. 추출된 `PIU/PIU.EXE`를 실행 파일로 사용합니다. data track 밖의 audio extent는 file cache에서 제외하고 MSCDEX/CD-DA가 담당합니다. 원본 ROM/CHD는 읽기 전용이며 Git 밖에 유지합니다.

PIU ROM-set targets separate asset-container decoding from guest execution. `TargetProfile::rom_set_id` selects the shared ZIP/CHD mount without title-ID branching. Pinned libchdr exposes CHD CD frames. For a multisession CHD with multiple data tracks, the project-owned ISO9660 reader selects the last data track. It maps CHT2/CHTR track types to user-data offsets: 0 for cooked `MODE1`/`MODE2_FORM1`, 16 for `MODE1_RAW`, and 24 for `MODE2_RAW`. It then discovers a signed extent-LBA bias from the root self-record so single-session and multisession data tracks share one path. The selected track's PVD, extent bounds, and unsupported sector layouts remain fail-closed. The extracted `PIU/PIU.EXE` remains the executable path. External audio extents are skipped by the file cache and remain available through MSCDEX/CD-DA. A deterministic build cache supplies the existing filesystem-based DOS VFS; original ROM/CHD files remain read-only and outside Git.

```mermaid
flowchart LR
    ROM[MAME ROM ZIP] --> CHECK[set validation]
    CHD[CHD v5 CD] --> LIB[libchdr sectors]
    LIB --> ISO[ISO9660 reader]
    ISO --> CACHE[build mount cache]
    CACHE --> VFS[DOS VFS]
    VFS --> GUEST[original PIU code]
```
# MSCDEX CHD CD audio

`pumpit1` CHD는 ISO9660 mount source와 가상 MSCDEX disc를 동시에 제공합니다. `media::ChdCdImage`가 CHT2/CHTR track 및 raw sector를 담당하고, execution trampoline이 원본 `INT 2Fh AX=1500h/1510h` request를 해석하며, 호환 이름을 유지한 `CdAudioWaveOut` 내부의 SDL3 audio stream이 CD-DA PCM 출력만 담당합니다. Glide gate 관찰은 ordinal별 count와 최초 인자를 누적합니다.

```mermaid
flowchart LR
    G["Guest INT 2Fh"] --> M["MSCDEX adapter"] --> C["ChdCdImage"]
    M --> A["CdAudioWaveOut / SDL3 stream"]
    C --> A
```

A PIU target CHD is both the ISO9660 mount source and a virtual MSCDEX disc. `media::ChdCdImage` owns track metadata and raw sectors, the execution trampoline adapts original `AX=1500h/1510h` requests, and the SDL3 audio stream inside the compatibility-named `CdAudioWaveOut` owns only CD-DA PCM output. Glide observation accumulates counts and first arguments per ordinal.
# PIU10 YMZ280B board sound

CD-DA가 배경 음악을 담당하는 것과 별개로, 코인·메뉴 효과음은 PIU10 ISA 보드의 Yamaha YMZ280B가 `roms/pumpit1.zip`의 `piu10.u9` 샘플 ROM에서 재생합니다. 책임은 네 계층으로 분리되어 있습니다. `assets::ExtractRomZipEntry`가 ZIP 엔트리를 CRC 검증과 함께 추출하고, `sound::LoadPiu10SampleRom`이 4 MiB `0xFF` 주소 공간에 배치하며, 플랫폼 공용 `sound::Ymz280bDevice`가 레지스터 파일·8보이스·ADPCM 디코드·믹싱을 88200 Hz 스테레오로 수행하고, Win32 backend `Ymz280bAudioOut`이 워커 스레드와 뮤텍스로 SDL3 stream에 밀어 넣습니다. 게스트 ABI 연결은 `piu10_sound_port`가 담당하며 ISA 16비트 버스의 바이트 레인 규칙에 따라 `0x02A0`을 칩 오프셋 0, `0x02A2`를 오프셋 1로 디코드합니다. 사운드 창은 JAMMA 입력 범위 안에 있으므로 `HandlePortIoInstruction`에서 입력 분기보다 먼저 처리하며, 레지스터 쓰기는 NOP 패치 없이 EIP만 전진시켜 매번 재트랩합니다. 이 보드의 sample ROM 초기화와 `0x02A0..0x02AF` 입력·EEPROM·YMZ 라우팅은 target profile의 `enable_piu_jamma_board`가 true일 때만 활성화되며, 현재 모든 `pumpit*` 프로파일만 true입니다.

```mermaid
flowchart LR
    G["Guest OUT 0x02A0/0x02A2"] --> P["piu10_sound_port<br/>byte-lane decode"]
    P --> B["Ymz280bAudioOut<br/>mutex + worker"]
    B --> D["sound::Ymz280bDevice<br/>8 voices, 88200 Hz"]
    R["ROM-set ZIP / piu10.u9"] --> S["Ymz280bSampleRom<br/>4 MiB, 0xFF fill"] --> D
    B --> SDL["SDL_AudioStream"]
```

Separately from CD-DA background music, coin and menu effects are played by the Yamaha YMZ280B on the PIU10 ISA board from the `piu10.u9` sample ROM inside the target ROM-set ZIP. Responsibilities are split across four layers: `assets::ExtractRomZipEntry` extracts the ZIP entry with CRC verification, `sound::LoadPiu10SampleRom` places it in a 4 MiB `0xFF`-filled address space, the platform-neutral `sound::Ymz280bDevice` owns the register file, eight voices, ADPCM decode, and mixing at 88200 Hz stereo, and the Win32 backend `Ymz280bAudioOut` pushes generated PCM into an SDL3 stream from a worker thread under a mutex. `piu10_sound_port` provides the guest ABI glue, decoding `0x02A0` as chip offset 0 and `0x02A2` as offset 1 per the ISA 16-bit byte-lane rule. Because the sound window lies inside the JAMMA input range, `HandlePortIoInstruction` handles it before the input branch, and register writes advance EIP and re-trap rather than being NOP-patched. Sample-ROM setup and JAMMA input, EEPROM, and YMZ routing in `0x02A0..0x02AF` are enabled only when `TargetProfile::enable_piu_jamma_board` is true; currently that means every `pumpit*` profile and no other profile.

## 부모 ROM 세트 자산 fallback

`TargetProfile::parent_rom_set_id`는 MAME ROM 세트의 직접 parent를 명시합니다.
CAT702 자산은 현재 ZIP의 `<current>.cat702`, 같은 ZIP의 `<parent>.cat702`, 형제
경로의 `<parent>.zip` 안 `<parent>.cat702` 순서로 조회하여 non-merged와 split ROM
구성을 모두 지원합니다. fallback은 ZIP 항목이 없을 때만 허용하며 읽기, 추출 또는
CRC 오류는 숨기지 않습니다. clone의 ZIP/CHD mount와 원본 실행 파일은 계속 clone의
`rom_set_id`를 사용합니다.

## Parent ROM-Set Asset Fallback

`TargetProfile::parent_rom_set_id` records the direct MAME ROM-set parent. CAT702
assets resolve in this order: `<current>.cat702` in the current ZIP,
`<parent>.cat702` in that ZIP, then `<parent>.cat702` in the sibling
`<parent>.zip`. This supports non-merged and split ROM layouts. Fallback is
allowed only for a missing member; read, extraction, and CRC errors remain
visible. A clone continues using its own `rom_set_id` for ZIP/CHD mounting and
original executable selection.

# PIU10 flash, MP3, CAT702 ISA board

Task 465부터 PIU10 frame-tail batch는 target 이름, 고정 EIP, LE object 번호나 data offset을
사용하지 않습니다. 현재 `OUT DX,AL` 주변의 bounded feeder-loop 명령을 좁게 decode하고
load/increment/store의 register def-use와 memory alias를 검증한 뒤, instruction operand와 branch
target에서 source cursor, available end, buffer, frame count/target과 주기적 service 경계를
추출합니다. 서로 독립인 cursor/count 갱신의 순서와 임시 register allocation은 고정하지
않으므로 `pumpito` 계열과 `pumpite`에서 확인된 동등한 compiler schedule을 같은 계약으로
처리합니다. batch는 추출한 경계 직전까지만 guest 상태를 commit하며, dependency, clobber,
shape나 runtime state가 맞지 않으면 원본 scalar loop를 그대로 실행합니다. Task 471부터는
`OUT DX,AL`이 register 보존 래퍼 안에 있는 경우에도 guest stack 반환 주소, 상대 `call`
대상, 호출 전 feeder 갱신과 반환 후 loop edge를 모두 검증하여 같은 계약으로 정규화합니다.
감사 계획은 service 경계뿐 아니라 현재 compressed inflight의 `0xE00` `DEMAND` 경계에서도
끊어 실제 scalar status polling과 같은 반환 지점을 보존합니다.

Since Task 465, PIU10 frame-tail batching uses no target name, fixed EIP, LE object number, or data
offset. It narrowly decodes bounded feeder-loop instructions around the current `OUT DX,AL`,
validates register def-use and memory aliases across load/increment/store operations, and derives
the source cursor, available end, buffer, frame count/target, and periodic-service boundary from
instruction operands and branch targets. Independent cursor/count update ordering and temporary
register allocation are not fixed, so the equivalent compiler schedules observed in the
`pumpito` family and `pumpite` share one contract. It commits guest state only up to the derived
boundary; any dependency, clobber, shape, or runtime-state mismatch preserves the scalar loop.
Since Task 471, an `OUT DX,AL` inside a register-preserving wrapper is normalized to the same
contract only after validating the guest-stack return address, relative `call` target, feeder
updates before the call, and loop edge after the return. Audit plans stop at both the service
boundary and the `0xE00` compressed-inflight `DEMAND` boundary so scalar status polling returns at
the same point.

`0x02D0..0x02DF`의 PIU10 ISA 보드는 `0x02A0..0x02AF`의 JAMMA/YMZ280B 보드와 별도 장치입니다. 플랫폼 공용 `hle::Piu10IsaBoard`가 20-bit flash 주소, 12-bit 목적지, `piu10.u8` read-only word access, MP3 status-source 계약과 optional CAT702 PIU 직렬 상태를 소유합니다. 실행 준비 계층은 target profile의 `enable_piu10_isa_board`가 활성화되면 `piu10.u8`을 추출하고, 별도 `enable_cat702`도 true일 때만 `<target>.cat702`를 요구하고 주입합니다. CAT702이 false여도 PIU10 flash, MP3와 DAC는 동작하며 CAT702 data-out bit는 0, data/clock/select 쓰기는 무시됩니다. 기본 내장 profile에서는 두 capability가 `pumpito`, `pumpitc`, `pumpitpc`, `pumpite`, `pumpitpr`, `pumpitpx`, `pumpit8`, `pumpitp2`, `pumpipx2`, `pumpitp3`, `pumpipx3`에 켜지고 `pumpit1`, `pumpit2`, `pumpit3`에는 꺼져 있습니다. 목적지 `0x008`의 압축 byte는 `sound::DecoderInputFifo`의 4 KiB 물리 SPSC ring으로 들어갑니다. FIFO의 atomic inflight는 guest 수락부터 parser/minimp3의 실제 소비까지 ring과 worker staging을 합친 모든 byte를 추적하며, `DEMAND`는 이 값이 MAS3507D 계약을 참고한 `0xE00` byte 논리 수위보다 낮을 때만 활성화됩니다. ring에서 staging으로 이동할 때는 inflight가 줄지 않고 parser cursor가 전진할 때만 줄어듭니다. Win32 `Piu10Mp3AudioOut` worker는 첫 세 개의 호환 MPEG header로 sync를 확정한 뒤 하나의 persistent upstream `minimp3` decoder로 frame을 연속 decode하고, S16 PCM을 SDL3 audio device stream에 직접 공급합니다. 공용 MPEG parser가 연속 header로 검증한 정확히 한 frame 길이만 minimp3에 넘겨 FIFO pop 끝의 불완전한 다음 header가 현재 frame sync를 무효화하지 않게 합니다. decode된 각 PCM frame의 시작 offset은 재생 시계와 비교하며, MAS3507D frame-sync 상태는 worker의 선행 decode 시점이 아니라 재생이 해당 offset에 도달할 때 전이합니다. 재생 시계(Task 740)는 SDL 장치가 가져간 byte 수(`total_pcm_bytes_put - SDL_GetAudioStreamQueued`)를 그대로 쓰지 않습니다. 그 값은 장치 buffer 단위(WSLg 17 ms, 다른 host 46 ms 이상)로 계단처럼 움직여 한 계단 안의 frame 경계가 한 호출에서 함께 토글되고 decoder도 계단 단위로 몰아서 채웠으며, guest의 demand와 frame 수도 같은 계단으로 움직였습니다. 시계는 직전 계단의 byte 수에서 PCM 속도로 올라가고, 뒤로 가지 않으며, 장치가 가져간 수를 넘지 않습니다. decoder는 이 시계 앞에 0.25초를 유지하도록 frame 하나씩 decode하고, 큐가 그 절반 아래로 떨어졌을 때만 한 번에 채웁니다. 검증된 feeder-loop shape에서는 현재 `OUT DX,AL` 뒤의 frame tail을 한 번에 ring으로 옮기고, 명령 operand에서 추출한 source cursor, frame count와 guest counter를 원본 byte loop와 동일하게 갱신합니다. batch enqueue는 inflight의 `0xE00` 논리 수위를 넘지 않고 byte path는 4 KiB까지 허용하여 512-byte stale-status race headroom을 유지합니다. 검증에 실패하거나 FIFO 여유가 부족하면 기존 byte 경로로 닫히므로 원본 frame 경계와 제어 흐름은 유지됩니다. `REPIU_PIU10_MP3_STREAM_AUDIT=1`은 모든 PIU10 capability 진단 실행에서 producer/consumer stream을 독립적인 4 KiB hash로 비교하며 기본값은 꺼져 있습니다. 다른 PIU10 접근은 일반 adapter와 계측을 유지합니다. MAME 구현은 계약 참고 자료일 뿐 코드는 포함하지 않습니다. 별도 `piu10.u9` YMZ280B 초기화는 이 capability와 독립적입니다.

```mermaid
flowchart LR
    Z["ROM-set ZIP"] -->|"piu10.u8"| D["hle::Piu10IsaBoard"]
    Z -->|"target.cat702 when enabled"| C
    G["Guest IN/OUT<br/>0x02D0..0x02DF"] --> W["PIU10 adapter / MP3 fast path"] --> D
    D --> F["read-only flash"]
    D --> M["MP3 ready status"]
    D --> C["CAT702 serial transform"]
    W --> Q["DecoderInputFifo<br/>4 KiB ring + inflight"] --> M3["encoded staging<br/>persistent minimp3"] --> S["SDL3 PCM stream"]
    Q -->|"inflight < 0xE00"| M
```

The PIU10 ISA board at `0x02D0..0x02DF` is separate from the JAMMA/YMZ280B board at `0x02A0..0x02AF`. Platform-neutral `hle::Piu10IsaBoard` owns the 20-bit flash address, 12-bit destination, read-only `piu10.u8` word access, MP3 status-source contract, and optional CAT702 PIU serial state. Setup extracts `piu10.u8` when `enable_piu10_isa_board` is true and requires `<target>.cat702` only when the independent `enable_cat702` capability is also true. With CAT702 disabled, flash, MP3, and DAC remain active, CAT702 data-out reads zero, and data/clock/select writes are ignored. Both capabilities default to enabled for `pumpito`, `pumpitc`, `pumpitpc`, `pumpite`, `pumpitpr`, `pumpitpx`, `pumpit8`, `pumpitp2`, `pumpipx2`, `pumpitp3`, and `pumpipx3`, and disabled for `pumpit1`, `pumpit2`, and `pumpit3`. Destination-`0x008` compressed bytes enter the 4 KiB physical SPSC ring in `sound::DecoderInputFifo`. Its atomic inflight count tracks every byte across the ring and worker staging from guest acceptance through actual parser/minimp3 consumption; `DEMAND` is asserted only while this value is below the MAS3507D-derived logical `0xE00` level. Moving bytes into staging does not reduce inflight, while parser-cursor advancement does. The Win32 `Piu10Mp3AudioOut` worker confirms sync from three compatible MPEG headers, continuously decodes frames with one persistent upstream `minimp3` instance, and sends S16 PCM directly to an SDL3 audio-device stream. It passes minimp3 exactly one frame length already validated through consecutive headers by the shared MPEG parser, so an incomplete next header at a FIFO-pop boundary cannot invalidate the current frame. Each decoded PCM frame's start offset is compared with a playback clock, and MAS3507D frame-sync changes when playback reaches that offset rather than when the worker decodes ahead. The clock (Task 740) does not use the pulled byte count (`total_pcm_bytes_put - SDL_GetAudioStreamQueued`) directly: that count moves in device-buffer steps (17 ms on WSLg, 46 ms or more on other hosts), so every frame boundary inside one step toggled in the same call, the decoder refilled a whole step at once, and the guest's demand and frame count moved in the same steps. The clock rises at the PCM rate from the count before the latest step, never runs backwards and never passes the pulled count. The decoder keeps a quarter second ahead of this clock one frame at a time, and refills at once only when the queue has dropped below half of that. For a verified feeder-loop shape, the HLE transfers the current frame tail in one ring operation and updates the source cursor, frame count, and guest counter derived from the instruction operands. Batch enqueue stops at the inflight logical `0xE00` level, while the byte path may use 4 KiB to retain 512 bytes of stale-status race headroom. It fails closed to the byte path on a shape, range, state, or FIFO-space mismatch, preserving original frame-boundary and control-flow logic. `REPIU_PIU10_MP3_STREAM_AUDIT=1` independently hashes producer and consumer streams in 4 KiB chunks for every PIU10-capable target and is off by default. Other PIU10 accesses retain the generic adapter and instrumentation. MAME is a contract reference only and no MAME code is included. Independent `piu10.u9` YMZ280B initialization remains outside this gate.

모든 PIU10 target profile의 MP3 시작 지연 기본값은 0 ms입니다. `REPIU_PIU10_MP3_LATENCY_MS`는 실행별로 0~500 ms 범위의 지연을 명시할 때만 적용됩니다. 플랫폼 공용 `sound::Dac3350aControl`은 목적지 `0x010`의 SDA/SCL에서 DAC3350A I²C AVOL transaction을 복원하고, MAME의 DAC3350A 계약과 같은 dB 계단을 linear gain으로 변환합니다. AVOL 0은 mute, 1은 -75 dB, `0x2C`는 0 dB입니다. Win32 backend는 좌우 gain의 평균을 thread-safe `SDL_SetAudioStreamGain`에 전달합니다. 이는 이미 queue에 들어간 PCM의 출력 gain에도 적용되지만 SDL queue, 압축 FIFO, MPEG/minimp3 상태와 frame-sync는 변경하지 않습니다. 실제 `pumpito`에서는 좌우 값이 같아 평균으로 인한 차이가 없습니다. pause 상태는 모델링하지 않습니다.

`REPIU_PIU10_DAC_AUDIT=1`은 각 DAC transaction에서 `Piu10Mp3AudioOut::Snapshot()`을 호출하여 미소비 SDL 입력 PCM byte와 현재 형식 기준 밀리초, audio device callback buffer 크기, compressed ring, worker의 decoder pending byte, 두 저장 위치를 포괄하는 compressed inflight, 누적 receive/decode와 frame-sync를 같은 로그 줄에 남깁니다. snapshot은 관측 전용이며 queue와 decoder 상태를 변경하지 않습니다. PCM 형식이 queue 도중 바뀌면 SDL은 입력 byte 총량만 제공하므로 `pcm-queued-ms`는 현재 형식 기준 근사치이고, device buffer는 현재 잔량이 아니라 한 hardware-feed chunk의 시간 범위입니다.

Every PIU10 target profile defaults MP3 startup latency to 0 ms. `REPIU_PIU10_MP3_LATENCY_MS` applies only when a per-run delay in the 0–500 ms range is explicitly requested. Platform-neutral `sound::Dac3350aControl` reconstructs DAC3350A I2C AVOL transactions from destination-`0x010` SDA/SCL and converts the MAME-equivalent DAC3350A dB steps to linear gain. AVOL zero is mute, 1 is -75 dB, and `0x2C` is 0 dB. The Win32 backend sends the mean of the left and right gains to thread-safe `SDL_SetAudioStreamGain`. This affects queued PCM at output time without changing the SDL queue, compressed FIFO, MPEG/minimp3 state, or frame-sync. Observed `pumpito` values have equal channels, so averaging introduces no difference. Pause is not modeled.

With `REPIU_PIU10_DAC_AUDIT=1`, every DAC transaction calls `Piu10Mp3AudioOut::Snapshot()` and appends unconsumed SDL input PCM bytes and duration in the current format, audio-device callback-buffer size, compressed-ring bytes, decoder-pending worker bytes, compressed inflight across both storage locations, cumulative receive/decode counters, and frame-sync to the same record. The snapshot is observation-only. If queued PCM spans a format change, SDL exposes only the aggregate input byte count, so `pcm-queued-ms` is an approximation in the current format; device-buffer duration describes one hardware-feed chunk rather than exact pending occupancy.
# AOT translation planning prototype

`runtime::BuildAotTranslationPlan`은 relocated DOS/4GW LE image의 entry/direct edge에서 reachable CFG를 Zydis로 복원하고 copy, direct relocation, HLE boundary, return, indirect exit를 분류합니다. 이 단계는 실행 경로를 바꾸지 않으며 `repiu_aot_probe`가 coverage와 planning time을 측정합니다.

```mermaid
flowchart LR
    LE["Relocated LE"] --> CFG["AOT CFG plan"]
    CFG --> CACHE["Code-cache image"]
    CFG --> HLE["HLE sentinels"]
    CFG --> INDIRECT["Indirect dispatch sites"]
```

`runtime::BuildAotTranslationPlan` recovers a reachable Zydis CFG from relocated DOS/4GW LE images and classifies copy operations, direct relocations, HLE boundaries, returns, and indirect exits. It does not alter execution yet; `repiu_aot_probe` measures coverage and planning time.

plan builder는 선택적 자기 계측을 가집니다(Task 330). `AotPlanBuildProfile` 포인터를
받으면 decode, record 생성, 분류, walk, sweep 단계를 공용 `runtime::ReadCycleCounter`로
귀속하고, `nullptr`이면 timestamp를 전혀 읽지 않습니다. 플랫폼 계층은 이 POD를 누적만
하므로 공용 파일에 Win32 헤더가 들어가지 않습니다. `repiu_aot_probe`의
`plan_build_bench_*` 그룹은 같은 코드를 Debug와 Release로 측정해 **빌드 구성 왜곡을
분리**합니다. 이 구분이 필요한 이유는 두 구성에서 단계 순위가 뒤집히기 때문입니다.

The plan builder carries optional self-attribution (Task 330): given an `AotPlanBuildProfile` it
attributes decode, record build, classify, walk, and sweep through the neutral
`runtime::ReadCycleCounter`, and reads no timestamp when the pointer is null. The platform layer
only accumulates that POD, so no Win32 header enters platform-neutral code. The probe's
`plan_build_bench_*` group measures the same code in Debug and Release to separate build
configuration distortion, which matters because the stage ranking inverts between them.

## AOT code-cache emission

`runtime::BuildAotCodeCacheImage`는 instruction-level plan에서 플랫폼 공용 비실행
byte image를 만듭니다. 일반 명령과 return을 보존하고 direct call/jump/Jcc를
`rel32`로 정규화하며, guest-address/cache-offset map으로 내부 edge를 해결합니다.
HLE 또는 간접 경계는 외부 sentinel fixup으로 남깁니다. basic-block 끝의 일반
copy 명령에는 명시적인 `rel32` fall-through를 추가하므로 cache layout이 guest의
선형 제어 흐름을 바꾸지 않습니다.

Task 630부터 조건 분기로 끝나는 블록의 not-taken edge도 같은 규칙을 따릅니다.
분기는 pending fall-through를 남기고, 실제로 바이트를 만드는 다음 명령이 그
target이면 인접성이 edge를 대신하므로 아무것도 추가하지 않습니다. target이
아니거나 뒤에 emit할 명령이 없으면 `rel32` fall-through를 명시적으로
추가합니다. 따라서 block 배치 순서가 guest 순서와 달라도 not-taken edge가
유지되며, fall-through가 물리적으로 다음인 image의 바이트는 바뀌지 않습니다.

Task 632부터 long mode는 jump table도 emit합니다. i386처럼 target 표를 cache
안에 복사하지 않고, guest의 표를 실행 시점에 읽어 target을 return thunk
resolver에 넘깁니다. 즉 indirect call 슬롯에서 return 주소 push만 뺀
형태입니다. 받는 형식은 `FF /4` + SIB(scale 4, base 없음, index가 ESP가 아님)
+ disp32이며, 선행 `2E`(CS) 하나는 무시합니다. LE fixup이 절대 주소를 이미
배치된 선형 주소로 바꾸므로 이 이미지에서 `CS:`는 데이터 참조에 무연산이고,
i386 슬롯도 같은 이유로 이 prefix를 버립니다. 그 밖의 형식은 거부되어 기존처럼
경계로 남습니다.

```mermaid
flowchart LR
    PLAN["Instruction plan"] --> EMIT["Two-pass emitter"]
    EMIT --> BYTES["Code-cache bytes"]
    EMIT --> MAP["Guest/cache map"]
    EMIT --> FIXUP["Resolved + external fixups"]
    FIXUP --> ABI["Execution ABI sentinels"]
```

`runtime::BuildAotCodeCacheImage` consumes instruction-level plan records and
builds a platform-neutral, non-executable byte image. It preserves ordinary
instructions and returns, normalizes direct call/jump/Jcc edges to `rel32`,
resolves internal edges through a guest-address/cache-offset map, and leaves HLE
or indirect boundaries as external sentinel fixups. Explicit `rel32` fall-through
links preserve guest linear control flow independently of cache layout.

Since Task 630 the not-taken edge of a block ending in a conditional branch
follows the same rule. The branch leaves a pending fall-through: if the next
instruction that actually emits bytes is its target, adjacency carries the edge
and nothing is added; otherwise, or when nothing follows, an explicit `rel32`
fall-through is written. The not-taken edge therefore survives a block order
that differs from guest order, and images whose fall-through is physically next
keep their bytes.

Since Task 632 long mode also emits jump tables. Rather than copying a table of
targets into the cache as i386 does, it reads the guest's own table at run time
and hands the target to the return-thunk resolver -- the indirect-call slot
without its return-address push. The accepted form is `FF /4` with a SIB of
scale four, no base, and an index that is not ESP, plus a disp32; one leading
`2E` (CS) is ignored. LE fixups have already rewritten absolute addresses into
placed linear ones, so `CS:` is a no-op on a data reference in this image, and
the i386 slot drops the same prefix for the same reason. Every other form is
refused and stays a boundary.

`platform::win32::PlaceWin32AotCodeCache`는 별도 Win32 cache allocation과
RW copy 후 RX 전환, instruction-cache flush, 양방향 guest/cache lookup을
담당합니다. Task 435부터 `dynamic`이 기본 backend이며, 정적 cache bridge와 live
arena에서 arbitrary-entry CFG를 덧붙이는 동적 번역을 함께 활성화합니다.
`REPIU_EXECUTION_BACKEND=legacy`는 회귀 대조군으로 남아 single-step 경로를
선택합니다. cache sentinel은 기존 VEH/HLE dispatcher로 복귀하며, 해결할 수 없는
target은 legacy single-step으로 fail-closed합니다.

The Win32 placement layer owns executable cache allocation, RW copy followed by
RX protection, instruction-cache flushing, and bidirectional guest/cache lookup.
Since Task 435 `dynamic` is the default backend, enabling both the static cache
bridge and the dynamic translation that appends an arbitrary-entry CFG from the
live arena; `REPIU_EXECUTION_BACKEND=legacy` remains as the regression control
and selects the single-step path. Cache sentinels return through the existing
VEH/HLE dispatcher, and unresolved targets fail closed to legacy single-step
execution.

## 실행 backend 명명 규칙 / Execution backend naming layers

세 이름이 자주 함께 나오지만 서로 다른 층을 가리킵니다. `dynamic` backend를
`REPIU_AOT_*` 노브로 조율하는 구조가 처음에는 어긋나 보이므로 여기서 정리합니다.

| 층위 | 이름 | 가리키는 것 |
|---|---|---|
| 실행 정책 | `legacy` / `dynamic` | 사용자가 `REPIU_EXECUTION_BACKEND`로 고르는 노브 |
| 하위 시스템 | `AOT` | 실행 전에 code cache를 계획·생성·배치하는 단계 |
| 번역 계층 | `DBT` | 그 cache 위에서 런타임에 동작하는 번역·dispatch |

`AOT`는 여전히 정확한 이름입니다. `dynamic` backend에서도 게스트 실행 **전에**
`BuildAotTranslationPlan` → `BuildAotCodeCacheImage` → `PlaceWin32AotCodeCache`가
수행되어 `AOT cache base/bytes/entry`를 남깁니다. backend 이름이 `dynamic`인
것은 실행 중에도 번역이 계속된다는 뜻이지, 정적 단계가 없다는 뜻이 아닙니다.

Task 425는 backend를 `legacy`와 `dynamic` 둘로 줄였습니다. 옛 이름 `aot`,
`aot-dynamic`, `aot-dbt`는 별칭이 아니라 거부이며, 지정하면 실행 전에 exit 1로
종료합니다.

The three names appear together often but denote different layers, and a `dynamic`
backend tuned through `REPIU_AOT_*` knobs reads as a contradiction until they are
separated: `legacy`/`dynamic` name the **execution policy** the user selects,
`AOT` names the **subsystem** that plans, builds, and places the code cache before
execution, and `DBT` names the **translation layer** operating on that cache at
runtime.

`AOT` remains accurate. Even under `dynamic`, `BuildAotTranslationPlan`,
`BuildAotCodeCacheImage`, and `PlaceWin32AotCodeCache` all complete **before** the
guest starts and log `AOT cache base/bytes/entry`. The backend being called
`dynamic` means translation continues during execution, not that the static stage
is absent.

Task 425 reduced the backends to `legacy` and `dynamic`. The old names `aot`,
`aot-dynamic`, and `aot-dbt` are rejected rather than aliased, exiting 1 before
execution.

동적 translation은 arena를 복사하지 않고 **직접 참조**합니다(Task 329).
`RelocatedRuntimeObject`는 소유 바이트(`memory`) 또는 외부 뷰(`external_bytes`) 중
하나를 담고 읽기 경로는 `RelocatedRuntimeObjectBytes` / `...ByteCount` 접근자를
통과하므로, 보이는 범위가 같아 plan은 이전 스냅샷 시절과 바이트 단위로 동일합니다.
외부 뷰의 안전성은 **동기 rendezvous로 guest가 차단되어 있다는 사실에 의존**하므로,
번역을 비동기로 바꾸려면 먼저 복사를 복원해야 합니다.

동적 translation, inline-cache patch, guest-page retirement는 guest가 VEH 경계에서
대기하는 동안 serialized host-stack worker가 수행합니다. AOT VEH는 native 실행
전에 near indirect `FF /2`, `FF /4`, `C3/C2`를 해석합니다. call은 guest
fallthrough를 push하고 return은 guest/cache target을 명시적으로 map하므로 native
return 주소와 guest return 주소를 추측으로 섞지 않습니다. segment-register
operand는 명시적인 HLE boundary입니다.

Dynamic translation references the arena directly instead of copying it (Task 329). A
`RelocatedRuntimeObject` carries either owned bytes or an external view, and read paths go
through the `RelocatedRuntimeObjectBytes`/`ByteCount` accessors, so the visible range is
unchanged and plans stay byte-for-byte identical to the snapshot era. The view's safety depends
on the guest being blocked by the synchronous rendezvous, so making translation asynchronous
must restore a copy first.

Dynamic translation, inline-cache patching, and guest-page retirement run on a
serialized host-stack worker while the guest waits at a VEH boundary. Before
native execution, the AOT VEH resolves near indirect `FF /2` calls, `FF /4`
jumps, and `C3/C2` returns. Calls push guest fallthrough addresses, returns map
guest or cache targets explicitly, and segment-register operands remain HLE
boundaries. The dispatcher never scans for a plausible return address.

## AOT-DBT 실행 정책 기반 / AOT-DBT execution-policy foundation

Task 276은 플랫폼 공용 `runtime::ExecutionBackend`에 실행 정책을 정의하고,
Task 425가 이를 `legacy`와 `dynamic` 둘로 정리했습니다. Win32 host, 실행 trampoline과
thread context가 같은 정책 값을 전달하므로 backend 문자열, 정적 cache 사용, 동적
append, DBT 전용 dispatch 기능을 한 곳에서 판정합니다. `dynamic`은 별도 실행기나 코드
복제가 아니라 기존 AOT planner/emitter/cache/worker/SMC/HLE를 공유하는 정책입니다.

backend가 둘뿐이므로 정적 cache 생성·동적 번역·HLE 직후 즉시 재진입은 모두 같은
조건이며, `ExecutionBackendUsesDynamicTranslation` 하나로 판정합니다.

첫 DBT increment는 cache sentinel의 HLE 명령이 완전히 emulate되어 EIP가 전진한 뒤,
기존 cache entry로 즉시 복귀할 수 있으면 다음 원본 명령의 TF single-step을 생략합니다.
Zydis preflight가 첫 control transfer 전의 등록된 HLE boundary, decode/read 실패와
64명령 상한을 거부합니다. 방금 처리한 명령이 segment register를 쓴 경우에도 selector
변경 뒤의 HLE 의미를 보존하기 위해 기존 TF bridge를 유지합니다. cache miss,
quarantine과 모든 검증 실패는 원본 single-step fallback으로 fail-closed합니다.

```mermaid
flowchart LR
    H["AOT HLE boundary handled"] --> P{"dynamic policy"}
    P -->|"no"| TF["existing TF bridge"]
    P -->|"yes"| S{"segment write / span preflight"}
    S -->|"unsafe"| TF
    S -->|"safe"| C{"existing cache entry"}
    C -->|"hit"| R["cache re-entry, TF off"]
    C -->|"miss"| TF
```

Task 276의 30초 graceful 관측에서는 즉시 복귀 `5,670/2,335`
시도/성공, fatal 0, DBT legacy fallback 0을 기록했습니다. 성공마다 TF 명령 하나를
제거하므로 같은 실행 내부 proxy는 약 1.8% 절감입니다. 초기화 시점 차이가 있는
단일 A/B의 원시 누적값은 wall-clock 향상으로 사용하지 않으며, 완전한 DBT의
return/indirect/cache-miss host dispatcher는 후속 범위입니다.

Task 289 Stage 1은 Task 264 segment-override guard의 live resolution을
`selector/base/limit/flags/policy`로 확장합니다. selector 0과 전체 linear range가 DOS
low-memory 안에 있는 descriptor는 cache site를 `INT3` HLE boundary로 유지하고, 정상
nonzero flat descriptor와 검증된 GS non-flat base-add descriptor만 guard 네이티브로
활성화합니다. segment load, DPMI descriptor 변경, DOS/LINEXE shadow selector 변경은
전체 fingerprint가 달라질 때 site를 재패치합니다. 60초 smoke에서 native/HLE site가
각각 누적 193,288/120,668, 실제 HLE exit 7,554, mismatch 0이었고 fatal/legacy fallback은
0이었습니다.

Task 289 Stage 2는 post-HLE cache miss를 번역하는 옵트인을 더했지만 60초 A/B에서 번역
시도가 0회여서 기본 OFF로 남았고, issue #22에서 스위치와 함께 삭제했습니다. cache miss의
번역은 Linux x64 long mode에서 첫 명령의 바이트가 다를 때(`non_identical_target`)만
일어납니다.

Task 276 defines the execution policy in the platform-neutral
`runtime::ExecutionBackend`, which Task 425 reduced to `legacy` and `dynamic`. The
Win32 host, trampoline, and thread context carry the same policy value, while
`dynamic` shares the existing planner, emitter, cache, worker, SMC coherency, and
HLE implementation rather than forking an executor. With only two backends,
building the static cache, translating dynamically, and re-entering immediately
after an HLE boundary are one condition, decided by
`ExecutionBackendUsesDynamicTranslation`.

The first DBT increment skips the next TF instruction only when a fully emulated
HLE boundary can re-enter an existing cache entry safely. Zydis preflight rejects
a registered HLE boundary before the first control transfer, decode/read failure,
or the 64-instruction cap. A just-emulated segment-register write also retains the
TF bridge. Cache misses, quarantine, and all validation failures fail closed to
the established original-code single-step path. A 30-second graceful observation
recorded 5,670/2,335 attempts/successes, zero fatal state, and zero DBT legacy
fallback; one skipped TF instruction per success is about 1.8% within-run proxy
reduction. Wall-clock improvement remains unconfirmed, and exception-free
return/indirect/cache-miss dispatch remains follow-up work.

Task 289 Stage 1 extends Task 264's live segment-override resolution to fingerprint
`selector/base/limit/flags/policy`. Selector zero and descriptors whose complete linear
range lies in DOS low memory keep an `INT3` HLE boundary; valid nonzero flat descriptors and
the proven GS non-flat base-add descriptor retain guarded-native execution. Segment loads,
DPMI descriptor changes, and DOS/LINEXE shadow-selector changes re-patch sites only when the
complete fingerprint changes. A 60-second smoke recorded cumulative native/HLE site counts
of 193,288/120,668, 7,554 actual HLE exits, zero mismatches, and zero fatal/legacy fallback.

Task 289 Stage 2 added an opt-in that translated post-HLE cache misses; a 60-second A/B
recorded zero translation attempts, so it stayed default off, and issue #22 deleted it with its
switch. A cache miss is translated only on Linux x64 long mode when the first instruction's
bytes differ (`non_identical_target`).

### AOT-DBT return miss host dispatch

Task 277은 `dynamic` return inline-cache miss tail만 `INT3` 대신 Win32 x86
host-stack thunk로 연결합니다. 플랫폼 공용 emitter는 guest source, miss 주소와
success/fallback continuation의 image-relative metadata를 만들고, Win32 placement와
dynamic append가 RW 구간에서 cache 절대 주소와 host thunk `rel32`를 해결합니다.
다른 backend는 기존 `popfd; INT3` layout을 그대로 사용합니다.

thunk는 `pushfd`/`pushad`로 guest 상태를 저장하고 entry trampoline이 보존한 host
ESP와 TEB stack bounds로 전환한 뒤 기존 `HandleAotReturnTransfer`를 호출합니다.
성공 continuation은 `ret imm16`으로 원본 `C3`/`C2` stack pop과 cache target 이동을
동시에 재현합니다. 검증·해석 실패는 `LEA ESP`로 DBT metadata만 제거하고 같은
return instruction의 provenance `INT3`로 돌아가므로 기존 VEH dispatcher가 처리합니다.

```mermaid
flowchart LR
    R["return inline-cache miss"] --> H["save guest state / host stack"]
    H --> D{"shared return resolver"}
    D -->|"success"| C["ret imm16 -> cache target"]
    D -->|"failure"| F["LEA metadata pop + INT3"]
    F --> V["existing VEH return dispatcher"]
```

15초 통제 실행에서 `dynamic`는 return host dispatch `5,507/849/4,658`
시도/성공/fallback, fatal 0, legacy fallback 0을 기록했습니다. 대조군은 당시 존재하던
`aot-dynamic` backend(Task 425에서 제거)로, 새 카운터 `0/0/0`과 기존 layout을
유지했습니다. 두 실행의 격리 EEPROM SHA-256은 원본과 같았습니다.

Task 277 connects only the `dynamic` return inline-cache miss tail to a Win32 x86
host-stack thunk. The platform-neutral emitter records image-relative source,
miss, and continuation metadata; Win32 placement and dynamic append resolve the
absolute cache address and host-thunk `rel32` in their existing writable windows.
Other backends retain the byte-identical `popfd; INT3` tail.

The thunk saves guest registers and flags, switches to the entry trampoline's
saved host ESP and TEB stack bounds, and invokes the established return handler.
A successful continuation uses `ret imm16` to reproduce the original `C3`/`C2`
pop and transfer to the cache target. Any failure removes only DBT metadata with
`LEA ESP` and reaches the provenance `INT3`, preserving the existing VEH fallback.
A controlled 15-second run recorded 5,507/849/4,658 attempts/successes/fallbacks,
zero fatal state, and zero legacy fallback; the contrast arm was the then-existing
`aot-dynamic` backend, removed in Task 425, which recorded 0/0/0 and kept its
existing layout. Isolated EEPROM hashes remained unchanged.

Task 280은 AOT-DBT 후속 순서를 네 단계로 고정합니다. (1) Task 276 HLE 후 기존
cache 즉시 복귀와 (2) Task 277 RET miss host dispatch는 완료됐습니다. 다음은
(3) RET fallback 원인의 배타적 계측·분류이며, 그 결과를 입력으로 (4) indirect
call/jump miss host dispatch를 설계합니다. 상세 완료 조건과 공통 A/B 기준은
`docs/design/20260724-280-aot-dbt-four-stage-roadmap.md`에서 관리합니다.

Task 280 fixes the AOT-DBT follow-up order into four stages: (1) the completed
Task 276 immediate existing-cache re-entry after HLE; (2) the completed Task 277
RET-miss host dispatch; (3) exclusive instrumentation and classification of RET
fallback causes; and then (4) host dispatch for indirect call/jump misses.
Detailed completion criteria and controlled A/B rules live in
`docs/design/20260724-280-aot-dbt-four-stage-roadmap.md`.

### AOT-DBT return fallback 원인 회계 / return fallback cause accounting

Task 281은 3단계로 RET fallback을 배타적 원인 하나로 회계합니다. 공개 실행 결과와
`ThreadContext`가 같은 고정 10칸 순서(site, state, opcode, stack, zero, HLE,
quarantine, non-guest, translation, unknown)를 공유하고, `RecordAotDbtReturnFallback`
하나만 total과 reason 하나를 함께 증가시킵니다. adapter는 site 검증 이전에 attempt를
기록하며, `HandleAotReturnTransfer`는 기본값이 `nullptr`인 선택적 출력 인자로 원인을
전달하므로 기존 호출자 동작은 그대로입니다. `INT3` 기반 provenance fallback은 변경
없이 유지됩니다.

격리 EEPROM `dynamic` 15초와 120초 hot phase 실행에서 fallback은
`5,413/5,413`과 `8,034/8,034` 모두 `quarantined target` 한 칸이었고, 나머지 8개 원인은
0이었습니다. quarantine은 자기 수정 페이지의 정확성 장치이므로 RET 경로에서 완화하지
않습니다. 같은 hot phase의 indirect boundary는 34,851회로 RET fallback의 약 4.3배이며,
이것이 4단계 대상을 indirect call/jump host dispatch로 확정한 근거입니다.

Task 281 implements Stage 3: the public execution result and `ThreadContext` share
one fixed ten-slot cause order, and a single helper increments the fallback total
together with exactly one reason. The adapter accounts the attempt before site
validation, and `HandleAotReturnTransfer` reports the cause through a defaulted
optional output parameter, so existing callers and the provenance `INT3` fallback
are unchanged. Two isolated-EEPROM runs classified every fallback as a quarantined
return target (5,413/5,413 and 8,034/8,034) with all other causes zero. Quarantine
protects self-modifying pages and therefore stays fail-closed on the RET path; the
same hot phase produced 34,851 indirect boundaries, about 4.3x the RET fallbacks,
which fixes Stage 4 on indirect call/jump host dispatch.

### AOT-DBT indirect call/jump host dispatch (Stage 4, 삭제됨) / Stage 4 (deleted)

Task 282는 `FF /2`/`FF /4` inline-cache miss tail을 host-stack thunk로 보내는 4단계를
구현했지만, 실제 `dynamic`에서 켜면 Glide attract 경로가 결정적으로 크래시해 기본 OFF
(`REPIU_AOT_DBT_INDIRECT`)로 남았습니다. issue #22에서 방출 꼬리, thunk, 핸들러, 이
경로에서만 무장되던 call-step 진단(`REPIU_AOT_DBT_CALL_STEP`, Task 285)과 함께
삭제했습니다. miss tail은 `popfd; INT3`이고, 반환 경로와 공용인 fallback 원인 enum
(`AotDbtDispatchFallbackReason`)과 VEH 경로의 `HandleAotIndirectTransfer`는 남아
있습니다. 과거 분석은 `docs/analysis/current-execution-frontier.md` Task 282 항목에
있습니다.

Task 282 implemented Stage 4, routing the `FF /2` / `FF /4` inline-cache miss tail to a
host-stack thunk, but enabling it live deterministically crashed the Glide attract path, so it
stayed default off (`REPIU_AOT_DBT_INDIRECT`). Issue #22 deleted the emitted tail, thunk and
handler together with the call-step diagnostic only this path armed (`REPIU_AOT_DBT_CALL_STEP`,
Task 285). The miss tail is `popfd; INT3`; the fallback-cause enum shared with the return path
(`AotDbtDispatchFallbackReason`) and the VEH path's `HandleAotIndirectTransfer` remain. The
historical analysis is under Task 282 in `docs/analysis/current-execution-frontier.md`.

### AOT-DBT 미해결 direct edge dispatch / unresolved direct-edge dispatch

Task 395는 완성된 정적 image에 target이 없는 direct call, direct jump, conditional branch,
block fall-through만 `dynamic` 전용 tail stub로 연결합니다. 이미 address map에 있는 target은
기존 `rel32` 직결을 유지하며, 다른 AOT backend는 image 생성을 fail-closed합니다. 따라서
정적 CFG의 보수적 과잉 탐색이 전체 AOT-DBT image를 폐기하지 않으면서도 일반 AOT의
완결성 계약은 바뀌지 않습니다.

stub은 dispatch 절대 주소와 guest target을 push하고 Win32 x86 host-stack thunk로
진입합니다. resolver는 site metadata를 검증한 뒤 공용 `ResolveAotTransferTarget`만
호출합니다. 성공 시 cache target으로 `ret`하며, 실패 시 metadata 한 칸을 제거하고 전용
INT3에 도달합니다. re-entry는 address map에 가짜 code entry를 만들지 않고 site의 fallback
offset으로 원래 guest target을 복원합니다. static placement와 dynamic append는 동일한
image-relative metadata를 patch·이동합니다.

```mermaid
flowchart LR
    E["unresolved direct edge"] --> S["AOT-DBT tail stub"]
    S --> H["host-stack thunk"]
    H --> R{"ResolveAotTransferTarget"}
    R -->|success| C["cache target"]
    R -->|failure| I["site INT3"]
    I --> V["existing VEH/TF guest path"]
```

Task 395 connects only direct calls, direct jumps, conditional branches, and block
fall-throughs whose targets are absent from a completed static image to an AOT-DBT tail
stub. Mapped targets remain direct `rel32` edges, while other AOT backends keep rejecting
incomplete images. The Win32 x86 thunk validates site metadata and calls the shared
`ResolveAotTransferTarget`; success returns to a cache target and failure reaches a
site-owned INT3. Re-entry recovers the original guest target from site metadata rather
than publishing a fake address-map entry. Static placement and dynamic append patch and
relocate the same image-relative metadata.
### AOT-DBT CALL/RET 결정적 진단 경계 / deterministic CALL/RET diagnostic boundary

Task 284는 indirect host-dispatch CALL 크래시를 추적하기 위해 Win32 전용
`aot_dbt_call_return_trace`를 추가합니다. 이 진단은
`REPIU_AOT_DBT_CALL_TRACE=1`에서만 켜지며, 공용 indirect/return handler에 들어온
dispatcher-visible event를 고정 256칸 ring과 누적 카운터에 기록합니다. CALL은
VEH/host origin, source, target, return address와 entry ESP를 보존합니다. RET 전체는
누적하지만, ring에는 반환주소 target으로 기존 CALL sequence를 확정할 수 있는 RET만
보존합니다. 해당 RET의 expected ESP는 `call_entry_esp - 4`이며 불일치는 별도 sticky
first-divergence에 남습니다.

```mermaid
flowchart LR
    C["dispatcher-visible CALL"] --> F["diagnostic call frame<br/>sequence + tuple"]
    R["dispatcher-visible RET"] --> M{"actual target ==<br/>recorded return?"}
    M -->|"yes"| E["retain correlated RET<br/>compare ESP"]
    M -->|"no"| O["count only<br/>correlation ambiguous"]
    F --> M
    E --> S["fixed final snapshot"]
    O --> S
```

이 계층은 resolver hot path에서 allocation, lock, 파일 I/O나 형식화 로그를 사용하지
않고, guest byte·code-cache layout·target·stack write·레지스터 결과를 변경하지
않습니다. guest thread 종료 뒤 `Win32MinimalExecutionAttempt`로 POD snapshot을
복사해 최종 로그에서만 출력합니다. inline-cache hit로 C++ resolver를 건너뛴 CALL/RET은
의도적으로 이 경계 밖입니다. 240초 A/B에서 크래시 전 30개 CALL tuple과 공통 26개
상관 RET tuple이 control과 모두 일치했으므로, 다음 관측 계층은 emitter의
inline-cache-hit/물리적 `C3` continuation입니다.

Task 284 adds the Win32-only `aot_dbt_call_return_trace` to diagnose the indirect
host-dispatch CALL crash. It is enabled only by `REPIU_AOT_DBT_CALL_TRACE=1` and records
dispatcher-visible events in a fixed 256-entry ring plus aggregate counters. CALLs retain
origin, source, target, return address, and entry ESP. Every RET is counted, but only a RET
whose actual target identifies a recorded CALL is retained and compared against
`call_entry_esp - 4`; the first correlated ESP mismatch is sticky.

The resolver hot path performs no allocation, locking, file I/O, or formatted logging, and
the layer changes no guest byte, cache layout, transfer target, stack write, or register
result. State is copied as POD into `Win32MinimalExecutionAttempt` after guest-thread exit
and printed only in the final log. C++-resolver-bypassing inline-cache hits are deliberately
outside the boundary. The 240-second A/B found all 30 pre-crash CALL tuples and all 26
common correlated RET tuples identical to control, so the next observation layer is the
emitted inline-cache-hit/physical `C3` continuation path.

Task 285는 선택한 host CALL에만 saved EFLAGS.TF를 켜 synthetic `C3` 직전·직후를
관측하고, DR0/DR1으로 cache/guest return continuation을 잡는 제한 probe를 추가합니다.
이 probe는 guest/cache byte를 패치하지 않으며 return watch 동안에만 새 native fast
path의 debug-register 사용을 억제합니다. sequence 27/30/33은 세 phase가 모두
일치했지만 sequence 56은 pre-C3 continuation이 `0xEB53DDDD`로 오염됐습니다.

이 결과는 adapter 수명 계약을 새로 확정합니다. placement의 dispatch-site 벡터 원소를
가리키는 포인터나 참조는 `HandleAotIndirectTransfer`/`HandleAotReturnTransfer` 호출을
가로질러 보존할 수 없습니다. 두 handler는 동적 번역을 append하여 같은 site 벡터를
재할당할 수 있기 때문입니다. resolver 진입 전에 필요한 site metadata를 값으로
snapshot해야 하며, resolver 뒤에는 placement 벡터 원소 포인터를 재사용하지 않습니다.

Task 285 adds a bounded probe that sets saved EFLAGS.TF only for selected host CALLs,
captures state before and after the synthetic `C3`, and uses DR0/DR1 to catch cache/guest
return continuations. It patches no guest or cache byte and suppresses new native-fast-path
debug-register ownership only during the return watch. Sequences 27/30/33 matched through
all three phases, while sequence 56 exposed a poisoned `0xEB53DDDD` pre-C3 continuation.

This establishes a new adapter lifetime contract: a pointer or reference into a placement
dispatch-site vector must never survive a call to `HandleAotIndirectTransfer` or
`HandleAotReturnTransfer`, because either resolver may dynamically append and reallocate
the same vector. Required site metadata must be snapshotted by value before resolver entry,
and no placement-vector element pointer may be reused afterward.

Task 286은 이 계약을 indirect와 RET host adapter에 적용했습니다. 두
`FindDispatchSite`는 더 이상 vector 원소 포인터를 반환하지 않고 caller-owned local
value에 site 전체를 복사합니다. fallback/success continuation을 포함해 resolver 전후의
모든 site field 접근은 이 snapshot만 사용합니다. sequence 56의 pre/post/return 상태는
전부 일치했고, calls-only 실제 실행은 수정 전 30~50초 Glide AV 없이 240초를
완주했습니다.

CALL host dispatch는 수명 결함 수정 뒤에도 opt-in입니다. 실측에서 33,741회 시도 중
성공은 60회(약 0.18%)였고, 단일 240초 실행의 progress 차이만으로는 성능 이득을
입증할 수 없습니다. 기본 활성화는 의미 있는 fallback 감소 또는 반복 성능 검증 뒤
별도 정책 결정으로 다룹니다.

Task 286 applies this contract to both indirect and RET host adapters. Each
`FindDispatchSite` now copies the complete site into caller-owned local storage instead of
returning a vector-element pointer. Every pre- and post-resolver field access, including
fallback and success continuations, uses only that snapshot. Sequence 56 matched through
pre-C3, post-C3, and return completion, and the calls-only workload ran for 240 seconds
without the former 30-to-50-second Glide AV.

CALL host dispatch remains opt-in after the lifetime fix. Only 60 of 33,741 measured
attempts succeeded (about 0.18%), and a single 240-second progress difference does not
establish a performance benefit. Default enablement is a separate policy decision after a
meaningful fallback reduction or repeated performance validation.

## AOT worker inline cache

반복되는 prefix 없는 legacy-32 `FF /2`, `FF /4`, `C3`, `C2 iw` 경계는
platform-neutral emitter가 guarded inline-cache slot과 patch metadata로 만듭니다.
첫 miss는 기존 VEH dispatcher에서 guest/cache target을 확인하고, serialized
Win32 worker가 cache를 RX에서 RW로 바꿔 target과 rel32 edge를 기록한 다음 guard를
마지막에 공개합니다. 이후 RX 복원과 `FlushInstructionCache`를 완료하기 전에는
guest가 재개되지 않습니다.

```mermaid
flowchart LR
    SLOT["Guarded call/jump/return slot"] -->|hit| EDGE["native rel32 cache edge"]
    SLOT -->|miss| VEH["existing VEH dispatcher"]
    VEH --> WORKER["serialized patch worker"]
    WORKER --> W["RX -> RW -> patch"]
    W --> X["RX + instruction-cache flush"]
    X --> SLOT
```

call hit는 guest fallthrough를 push하며, return hit는 `[ESP]`가 학습한 guest
return과 일치할 때만 `LEA`로 원래 pop을 수행합니다. operand/return 값이 바뀌거나
encoding을 안전하게 변환할 수 없으면 기존 dispatcher로 fail-closed합니다.
code cache는 RWX가 되지 않으며, 현재 page-wide protection 전환은 단일 guest
실행 thread 전제를 사용합니다.

Prefix-free legacy-32 `FF /2`, `FF /4`, `C3`, and `C2 iw` boundaries are emitted
as guarded slots with platform-neutral patch metadata. The existing VEH resolves
the first miss, and a serialized Win32 worker performs RX-to-RW patching, restores
RX, and flushes the instruction cache before the guest resumes. Calls preserve
guest fallthrough addresses; returns bypass the dispatcher only when the guarded
guest return value matches. Unsupported or changed values fail closed. The cache
never uses RWX, and the current page-wide protection transition assumes one guest
execution thread per loader process.

Task 273부터 간접 call/jump와 return site는 공통 `entries` 메타데이터로 최대 4개의
target을 기억합니다. 각 활성 guard의 불일치는 다음 entry compare로 이어지고 마지막
entry만 공통 miss tail로 이동합니다. worker는 같은 target 재사용, 첫 빈 슬롯 채움,
round-robin 교체 순으로 entry를 선택합니다. 동적 image append는 모든 entry offset을
재배치하고, guest page retire는 해당 page를 target으로 가진 모든 entry guard를
miss 형태로 되돌립니다. 기존 단일 슬롯 필드는 entry 0 offset을 복제해 호환성을
유지합니다.

Since Task 273, indirect call/jump and return sites retain up to four targets through
shared `entries` metadata. A mismatching active guard chains to the next entry compare,
and only the final entry reaches the common miss tail. The worker refreshes an existing
target, fills the first empty entry, then round-robin replaces. Dynamic append relocates
every entry offset, while guest-page retirement resets every guard targeting that page.
Legacy single-entry fields mirror entry zero for compatibility.

Task 274부터 간접 call/jump cache의 entry 수는 정적 image 생성 시 결정되는 명시적
build policy입니다. Win32 host는 `REPIU_AOT_INDIRECT_CACHE_SLOTS=1|4`를 읽고, 이 값을
정적 배치와 이후의 모든 동적 append에 동일하게 전달합니다. 기본값은 4이며 잘못된 값은
실행 전에 거부합니다. 이 선택은 emitter layout만 바꾸고 guest 코드, target 해석, patch
순서와 page-retirement coherence는 바꾸지 않습니다. 통제 성능 측정은 각 실행에 격리된
`REPIU_EEPROM_PATH` 사본을 제공하고 shared live telemetry의 window-open, texture, draw,
swap one-shot milestone을 사용합니다.

Since Task 274, the indirect call/jump entry count is an explicit build policy selected when
the static image is emitted. The Win32 host parses `REPIU_AOT_INDIRECT_CACHE_SLOTS=1|4` and
propagates it through static placement and every later dynamic append. Four remains the
default, and invalid values fail before execution. The policy changes only emitter layout;
guest code, target resolution, patch ordering, and page-retirement coherence remain shared.
Controlled measurements isolate persistent state with `REPIU_EEPROM_PATH` and use one-shot
window-open, texture, draw, and swap milestones from shared live telemetry.

Task 481부터 DBT host-stack return miss는 placement가 소유하는 site별 다양성 상태를
갱신합니다. 16회 이상 miss와 8개 이상의 서로 다른 guest return target이 확인된 site는
megamorphic으로 고정 분류하고, 그 호출부터 PIC 재패치만 생략합니다. 기존 entry와 guard는
그대로 두므로 이미 학습된 target hit는 계속 native edge를 사용하고, 나머지 target은 같은
resolver의 target resolution·stack pop·success continuation을 거칩니다. 상태가 site 배열과
동기화되지 않으면 기존 patch를 수행하므로 정책은 정확성 전제조건이 아닙니다. indirect
call/jump와 VEH return은 이 정책의 범위 밖입니다.

Since Task 481, each DBT host-stack return miss updates per-site diversity state
owned by the placement. A site with at least sixteen misses and eight distinct
guest return targets is permanently classified as megamorphic, and only PIC
repatching is skipped from that call onward. Existing entries and guards remain,
so learned hits still take their native edges while other targets use the same
resolver target resolution, stack pop, and success continuation. Unsynchronized
state fails open to patching, making the policy an optimization rather than a
correctness prerequisite. Indirect call/jump and VEH returns are out of scope.

Task 482부터 return handler에는 opt-in stage 계측이 있습니다.
`REPIU_AOT_RETURN_STAGE_PROFILE`이 켜진 실행에서만 DBT adapter와 VEH 경로가 각각 outer
창을 열고, 그 안에서 entry validation, guest stack target read와 call/return bookkeeping,
target classification과 resolution, megamorphic policy와 patch, guest continuation 다섯
단계를 서로 겹치지 않게 측정합니다. 같은 창에서 stage 합을 뺀 residual도 함께 누적하므로
stage 경계가 실제 비용을 놓치면 residual이 자랍니다. adapter와 resolver가 한 return을 두 번
세지 않도록 outer 창은 depth로 보호합니다. 계측이 꺼져 있으면 두 scope 모두 분기 하나만
지나며 timestamp를 읽지 않습니다. 종료 요약은 stage별 cycles/counts/max, outer 대비
coverage, residual과 Task 481 정책 site 중 관측 상위 16개를 함께 출력합니다. 이 계측은
게스트 레지스터·메모리, cache layout, 해석된 target, patch 결정을 바꾸지 않으므로 A/B의
대상이 아니라 다음 최적화 대상을 고르기 위한 근거입니다. 절차는
[return stage 귀속 가이드](docs/guides/return-stage-attribution.md)에 있습니다.

Since Task 482, the return handler carries opt-in stage instrumentation. Only while
`REPIU_AOT_RETURN_STAGE_PROFILE` is set do the DBT adapter and the VEH path open an outer
window, inside which five mutually exclusive stages are measured: entry validation, the guest
stack target read with call/return bookkeeping, target classification and resolution, the
megamorphic policy and patch, and guest continuation. The residual of that same window is
accumulated beside them, so boundaries that miss real cost show up as a growing residual. Outer
windows are depth-guarded, so the adapter and the resolver never count one return twice. With
the instrument off, both scopes pass one branch and read no timestamp. The shutdown summary
reports per-stage cycles, counts, and maxima, coverage against the outer window, the residual,
and the sixteen most-observed Task 481 policy sites. It changes no guest register or memory, no
cache layout, no resolved target, and no patch decision, so it is evidence for choosing the next
optimization rather than a subject of A/B measurement. The procedure is in the
[return-stage attribution guide](docs/guides/return-stage-attribution.md).

## Megamorphic direct-return table / Megamorphic direct-return table

Task 499부터 return inline cache의 miss 경로 **앞**에 probe를 emit합니다
(A/B 통과 후 승격되어 `dynamic` backend에서 **항상 켜짐** — 끄기 스위치
`REPIU_AOT_DIRECT_RETURN_TABLE`은 #20에서 제거). probe는 게스트 return target을 공용
direct-mapped memo table에서 찾아, 적중하면 게스트 return 슬롯을 cache target으로 덮어쓰고
**원본 RET과 같은 스택 효과로** 돌아갑니다. host 전환도, PIC 재패치도 없습니다. 실패하면
기존 miss 시퀀스로 그대로 떨어집니다.

`miss_cache_offset`은 그대로 popfd를 가리킵니다 — push되는 miss address, adapter의 site
조회 키, `IsAotInlineCacheMiss`의 Task 479 인덱스가 모두 그 값을 공유하기 때문입니다.
대신 PIC guard만 probe를 향하며, guard target은 emitter·patcher·retirement의 guard reset
세 곳이 `AotInlineCacheGuardTargetOffset()` 하나로 공유합니다.

정확성은 새 불변식을 만들지 않습니다. table 적중은 4-entry PIC 적중과 의미가 같으므로,
지켜야 할 것은 "항목이 PIC 항목과 같은 조건에서만 살아 있는가" 하나입니다. 삽입은 host가
`ResolveAotTransferTarget`으로 검증한 `kActiveHit` 결과만 받고(Glide gate direct target과
dynamic translation 결과는 제외), 무효화는 `ResetInlineCacheGuardsTargetingPage`가 PIC
guard를 되돌리는 자리에서 table 전체를 지웁니다. 그 지점은 worker 스레드지만 게스트
스레드가 retirement rendezvous로 정지해 있으므로 lock도 generation stamp도 필요 없습니다.
적중 경로는 결과를 전역이 아니라 **스택 슬롯**으로 전달합니다 — 비동기 타이머 주입이
store와 jump 사이에 끼어들 창을 없애기 위해서입니다.

Since Task 499 a probe is emitted **ahead of** the return inline cache's miss path
(promoted after its A/B and **always on** for the `dynamic` backend — the
`REPIU_AOT_DIRECT_RETURN_TABLE` kill switch was removed in #20). It looks the guest return target up in a shared
direct-mapped memo table and, on a hit, overwrites the guest return slot with the cache target and
returns with **the original RET's own stack effect** — no host transition and no PIC repatch. A
miss falls straight through into the existing sequence.

`miss_cache_offset` keeps pointing at the popfd, because the pushed miss address, the adapter's
site lookup key, and the Task 479 index behind `IsAotInlineCacheMiss` all share that value. Only
the PIC guards point at the probe, through the single `AotInlineCacheGuardTargetOffset()` helper
shared by the emitter, the patcher, and the retirement guard reset.

Correctness adds no new invariant: a table hit is semantically identical to a four-entry PIC hit,
so the only rule is that an entry lives exactly as long as a PIC entry would. Insertion accepts
only `kActiveHit` resolutions the host validated through `ResolveAotTransferTarget`, which excludes
Glide-gate direct targets and freshly translated code, and invalidation clears the whole table
where `ResetInlineCacheGuardsTargetingPage` resets PIC guards — a worker-thread point at which the
guest thread is already blocked for the retirement rendezvous, so no lock and no generation stamp
are required. The hit path carries its result in a **stack slot** rather than a global, closing the
window in which an asynchronous timer injection could overwrite it between the store and the jump.

## 플랫폼 분리와 Linux 빌드 / Platform split and the Linux build

Task 501부터 `repiu_exe`의 소스는 두 부류로 나뉘어 있었습니다. 공용 코어(3d-17 시점 57개)는
모든 플랫폼에서, `src/platform/win32`(81개)는 `if(WIN32)` 안에서만 빌드됐습니다.
(**당시 경로입니다. Task 520이 그 트리를 `src/engine/`으로 옮겼습니다.**)
**Task 503d-17부터 그 구분이 없습니다** — 실행 엔진 소스도 모든 플랫폼에서 빌드됩니다. 새 라이브러리
타깃을 만드는 대신 조건부 소스로 나눈 것은 링크와 include 배선을 다시 하지 않기
위해서입니다.

측정이 이 구조를 뒷받침합니다 — 공용 코어는 Win32 헤더를 하나도 include하지 않고, MSVC
전용 구문이 없으며, 32비트 포인터를 가정하지 않고, GCC 13에서 C++20으로 오류 없이
컴파일됩니다. 반면 Win32 계층은 `CONTEXT` 270곳, `VirtualProtect` 47곳, `fs:[...]`
어셈블리 28곳으로 결합도가 높습니다.

Task 502부터 런처도 Linux에서 빌드됩니다. `repiu_launcher`는 `src/host/linux/main.cpp`를
진입점으로 두고 런처 화면을 Windows와 공유하며, Windows 쪽 복귀 루프와 자식 프로세스는
가져오지 않습니다 — 둘 다 Win32 고유의 주소 공간 선점 순서에서 나온 것이라 Linux에도 같은
제약이 있는지는 실행 엔진이 생긴 뒤에 판단합니다. 롬셋을 고르면 엔진이 아직 없다고 알리고
종료합니다.

Task 503a부터 게스트 레지스터 상태는 `repiu::platform::GuestCpuContext`라는 이름을 갖습니다.
Windows에서는 `CONTEXT`의 별칭이라 기존 900여 곳의 필드 접근이 그대로 컴파일되고, 그 밖의
플랫폼에서는 **같은 필드 이름**을 가진 구조체입니다. 필드 이름을 유지하는 것이 요점입니다 —
호출부를 새 접근자 API로 고치는 편집 자체가 이식에서 가장 큰 회귀 원인이 되기 때문입니다.
플랫폼 차이는 `src/platform/linux/x86/guest_cpu_context.cpp`의 `ucontext_t` 변환 두 함수로
모입니다. `ContextFlags`는 Linux에서 무시되고, `FloatSave`는 glibc `_libc_fpstate`가 FSAVE
이미지 그대로라 필드 대 필드로 옮겨지며, 디버그 레지스터는 항상 0입니다 — Linux 사용자
공간은 자기 스레드의 디버그 레지스터를 쓸 수 없어 이를 쓰는 `native_linear_span`은 비활성입니다.

Task 549부터 x86-64 host에서의 write-back 규칙이 명시됩니다. `GuestCpuContext`는 모든
host에서 고정 32비트 계약이므로, 이 구조체를 통한 편집은 host register의 **low half만**
정하고 상위 절반은 host가 두었던 값을 유지합니다. 상위 절반을 0으로 미는 것은 이 host에서
안전한 절삭이 아니라 손실입니다 — 실행 중인 page와 fault가 일어나는 page가 4 GiB보다 훨씬
위에 있어서, 잘린 RIP로 재개하면 매핑된 적 없는 주소로 돌아갑니다. 같은 이유로 fault
handler가 planted `int3` 앞 바이트를 확인할 때는 `Eip`가 아니라 host context의 RIP를
사용합니다.

Task 503b부터 가상 메모리도 `repiu::platform`에 있습니다 — `ReserveMemory`,
`CommitMemory`, `ProtectMemory`, `ReleaseMemory`, `QueryMemory`, `IsRangeReadable`,
`FlushInstructionCacheRange`, `SystemPageSize`. 이 API는
`Virtual*`를 그대로 흉내 내지 않고 **호출부 112곳이 실제로 묻는 것**을 묻습니다. 보호
비트마스크 대신 `readable`·`writable`·`executable`을 돌려주므로, 같은 판정을 하던 손으로 쓴
분류기 다섯 개가 하나로 줄어듭니다. Linux 백엔드는 `mprotect`가 이전 보호값을 알려주지 않아
자기가 설정한 값을 그림자 테이블에 기록하고, rePIU가 만들지 않은 주소만
`/proc/self/maps`로 넘깁니다 — 게스트 스토어마다 파일을 파싱할 수는 없기 때문입니다.
커밋이 `ProtectMemory`와 별도 함수인 이유는 호스트가 갈리기 때문입니다: Linux는 `mprotect`로
충분하지만 Windows의 `VirtualProtect`는 예약만 된 페이지에서 실패합니다.

Task 506부터 AOT 코드 캐시와 게스트 페이지 coherence도 같은 계층만 사용합니다. 코드 캐시는
예약·방출과 RW↔RX 보호 전환을 공용 API로 수행하고, 동적 append·인라인 캐시 패치·페이지
retirement 뒤에는 `FlushInstructionCacheRange`를 호출합니다. x86에서는 데이터 쓰기와 명령
fetch가 일관되므로 플러시는 의도적으로 무동작에 가깝지만, 코드 수정 뒤 실행한다는 호출부의
계약은 보존합니다. 게스트 write-watch가 저장하는 이전 보호도 OS 비트마스크가 아니라
`MemoryProtection`입니다. 이 구조로 Linux 기본 `dynamic` backend가 같은 원본 x86 실행 경로를
사용하며, WSLg `pumpit1`에서 버퍼 스왑과 non-black 픽셀이 확인되었습니다.

Task 503c부터 폴트 전달도 `repiu::platform`에 있습니다. Windows는 VEH, Linux는 `sigaction`이
같은 `FaultKind`·`FaultEvent`를 만들고, 콜백이 레지스터를 편집한 뒤 재개 여부를 돌려줍니다.
Windows에서 `GuestCpuContext`가 `CONTEXT`이므로 콜백은 **커널이 준 구조체를 직접 편집**하며
복사가 없습니다. Linux는 `sigaltstack`으로 자기 스택에서 돌고 — 게스트 스택이 손상됐을 때도
핸들러가 살아야 하기 때문입니다 — `SA_NODEFER`로 중첩을 허용합니다.

`int3`의 `Eip` 규약은 계층이 통일합니다. Windows는 `int3` 바이트를, Linux는 다음 명령을
가리키는데, 엔진이 `Eip`의 바이트를 읽어 경계를 판별하므로 **Linux 쪽을 되감아 Windows 규약**에
맞춥니다. 그래서 규칙이 양쪽에서 하나입니다 — `Eip`는 `int3`를 가리키고, 지나가려면 핸들러가
직접 전진시킵니다.

Task 503d부터 Linux 바이너리는 **non-PIE로, 텍스트 세그먼트를 0x40000000에 올려** 링크합니다.
두 가지가 동시에 걸립니다 — 손으로 쓴 thunk는 전역을 직접 주소로 읽는데 PIE에서는 그것이 텍스트
재배치가 되고, 텍스트 세그먼트가 쓰기 가능해지면 페이지 보호로 SMC를 감지하는 방식이 무너집니다.
반대로 평범한 `-no-pie`는 i386 이미지를 0x08048000에 올리는데 그 주소는 게스트 재배치 범위
0x01000000~0x09000000 안입니다. 세그먼트를 들어 올리면 그 범위가 비워집니다.

다섯 개 디스패치 thunk는 부르는 resolver만 다르므로 Linux에서는 복사본 다섯이 아니라
`src/platform/linux/x86/stack_bridge.inc.S`의 **매크로 하나**입니다. MSVC 원본과 줄 단위로 대조되도록
Intel 문법으로 씁니다. Windows가 thunk마다 하는 `fs:[4]`·`fs:[8]` 조작(28곳)은 Linux 매크로에
**없습니다** — 커널이 시그널을 `sigaltstack`으로 준 스택에 얹고 중단된 스택이 어디인지 묻지 않기
때문이며, 이는 전환된 스택 위에서 폴트를 일으키는 probe로 확인했습니다.

Task 503d-16부터 트램폴린이 게스트로 들어가는 세 진입점도 GAS에 있습니다 —
`src/platform/linux/x86/guest_stack_switch.S`의 `CallGuestEntryWithStack`(스택 전환 자체)과 폴트
복귀 둘입니다. `StackSwitchCallState`의 필드 오프셋은
`include/repiu/platform/guest_stack_switch.h`에 `#define`으로 한 번만 두고, `.S`가 C
전처리기를 거치므로 어셈블리 두 벌과 C++ `static_assert`가 같은 숫자를 읽습니다.

다섯 thunk와 달리 이 진입점에는 `fxsave`가 없습니다. 부르는 것이 호스트 resolver가 아니라
게스트이고, i386 System V ABI는 호출 경계에서 x87 스택이 비어 있을 것을 요구하므로 건너갈
호스트 상태가 없습니다. 반대로 `RecoverGuestStackException`의 세그먼트 복원은 Linux에서
**유일한** 복원 경로입니다 — `StoreGuestCpuContext`가 세그먼트 레지스터를 되쓰지 않기
때문입니다.

어셈블리가 이름으로 읽는 전역 열한 개는 `src/platform/guest_stack_switch_state.cpp`에 모여
있고, 이 파일은 모든 호스트에서 빌드됩니다. 그래서 Linux 어셈블리가 트램폴린 본체보다 먼저
링크됩니다 — 엔진 코드가 Linux에서 컴파일만이 아니라 **링크된** 첫 사례입니다.

환경 블록 **전체 열거**는 `ForEachEnvironmentEntry`로 계층에 있습니다. 변수 하나를 읽는 3d-9의
`ReadEnvironmentSetting`과 달리 DOS 환경 블록은 전부를 복사해야 하고, Windows 백엔드는 CRT
사본(`_environ`)이 아니라 `GetEnvironmentStringsA`로 프로세스 환경을 읽습니다 — 그 둘은 시작
이후의 `SetEnvironmentVariable`에서 갈립니다. 쓰기 쪽 `PublishEnvironmentSetting`은 반대로
Windows에서 `_putenv_s`여야 합니다. 이 계층은 읽기를 두 군데(`std::getenv`와
`GetEnvironmentStringsA`)에서 하므로, 둘 다 갱신하는 함수만 답이 됩니다.

Task 503d-17부터 **Linux에서 `repiu` 로더가 링크됩니다.** 진입점은 Windows와 같은
`src/host/win32/main.cpp`(Task 731부터 `src/host/loader/main.cpp`)이고, 자식 프로세스 재실행은 `repiu::platform::RunChildProcessAndWait`로
갈라집니다 — Windows는 `CreateProcessA`, Linux는 `posix_spawn`입니다. Task 500이 이 재실행을
만든 이유(GPU 드라이버의 주소 공간 선점)가 Linux에도 해당하는지는 **아직 측정하지
않았습니다.** 무한 대기 표기도 중립 상수로 옮겼고, Windows 대기 옆의 `static_assert`가
`INFINITE`와 같은 값임을 고정합니다.

Task 503d-18부터 스레드도 `repiu::platform`에 있습니다 — `CreateHostThread`,
`QueryHostThread`, `JoinHostThread`, `CloseHostThread`. 진입점은 `std::uint32_t(void*)`로,
Windows의 `DWORD WINAPI(LPVOID)`도 POSIX의 `void*(void*)`도 아닌 호출부의 모양입니다.
`QueryHostThread`가 **기다리지 않고** 답하는 것이 요점입니다 — 호스트 폴 루프는 질문 사이에
Glide 명령을 펌프하고 타이머 틱을 전달해야 합니다.

`running`과 `exit_code`를 따로 답하는 것도 의도입니다. Windows의 `GetExitCodeThread`는 도는
스레드에 259를 돌려주는데 259는 적법한 종료 코드이기도 해서, `!= STILL_ACTIVE` 판정은 그
값으로 끝난 스레드를 영원히 도는 것으로 읽습니다. Windows 백엔드는 길이 0의 대기로 먼저
가릅니다. `TerminateThread`에는 Linux 대응물을 만들지 않습니다 — 그것은 최후 수단이고, 그 앞의
우아한 경로(정지 → `RecoverToHost` → 재개)가 Linux에서는 시그널로 같은 일을 하므로 유일한
경로가 됩니다.

`repiu_core_probe`는 플랫폼에 의존하지 않는 probe만 담아 **양쪽 OS에서 빌드**됩니다.
구성원은 문자열 검색이 아니라 **컴파일과 링크로** 정합니다 — probe는 include 디렉터리를
통해 `dos/dos_int21_services.h` 같은 상대 경로로 Win32 계층에 닿을 수 있고, 그것은
grep에 잡히지 않습니다. 아키텍처는 i386입니다. 게스트의 32비트 코드를 같은 프로세스에서
네이티브 실행하므로 호스트도 32비트여야 합니다.

Since Task 501 the `repiu_exe` sources were split in two: the platform-neutral files that build
everywhere — 57 as of 3d-17 — and the 81 under `src/platform/win32` (**the path at the time;
Task 520 moved that tree to `src/engine/`**) that built only inside
`if(WIN32)`. **Since Task 503d-17 that split is gone**: the execution engine builds everywhere too. Conditional
sources were chosen over a second library target to avoid rewiring every link and include
relationship. Measurement backs the split: the neutral core includes no Win32 header, uses no
MSVC-specific construct, assumes no 32-bit pointer, and compiles under GCC 13 with C++20 without
error, while the Win32 layer carries 270 `CONTEXT` uses, 47 `VirtualProtect` calls, and 28
`fs:[...]` assembly sites. `repiu_core_probe` collects the platform-independent probes and builds
on both systems; its membership is decided by compiling and linking rather than by searching for
strings, because a probe can reach the Win32 layer through an include directory with a relative
name like `dos/dos_int21_services.h`. The architecture is i386, because the guest's 32-bit code
runs natively in the host process.

Since Task 503a the guest's register state is named `repiu::platform::GuestCpuContext`: an alias for
`CONTEXT` on Windows, so the roughly 900 existing field accesses compile unchanged, and a structure
with the same field names elsewhere. Keeping the names is the point, because rewriting call sites
onto a new accessor API would itself be the port's largest source of regressions. The platform
difference collapses into the two `ucontext_t` conversions in
`src/platform/linux/x86/guest_cpu_context.cpp`. `ContextFlags` is ignored on Linux; `FloatSave` converts
field for field because glibc's `_libc_fpstate` is the same FSAVE image Windows calls
`FLOATING_SAVE_AREA`; and the debug registers are always zero, since Linux user space cannot write
its own thread's, which is why `native_linear_span` stays disabled there.

Since Task 549 the write-back rule on an x86-64 host is explicit. `GuestCpuContext` is a fixed
32-bit contract on every host, so an edit through it settles the **low half** of a host register and
leaves the half above it as the host had it. Zeroing that upper half is a loss rather than a safe
truncation here: the pages this process executes on and faults in sit far above 4 GiB, so resuming
from a truncated RIP returns to an address that was never mapped. For the same reason the fault
handler finds the byte before a planted `int3` through the host context's RIP rather than through
`Eip`.

Since Task 503b virtual memory lives in `repiu::platform` as well — `ReserveMemory`, `CommitMemory`,
`ProtectMemory`, `ReleaseMemory`, `QueryMemory`, `IsRangeReadable`, `FlushInstructionCacheRange`,
and `SystemPageSize`. The API does not mirror the
`Virtual*` shapes; it asks what the 112 call sites actually ask, returning `readable`, `writable`,
and `executable` instead of a protection bitmask, which collapses five hand-written classifiers of
that same question into one. The Linux backend records the protections it sets in a shadow table,
because `mprotect` does not report what it replaced and parsing `/proc/self/maps` on every guest
store is not viable; only addresses rePIU did not map fall through to that file. Committing is a
separate function from protecting because the hosts diverge: `mprotect` suffices on Linux, while
Windows' `VirtualProtect` fails on reserved-but-uncommitted pages.

Since Task 506 the AOT code cache and guest-page coherence use only that layer as well. Cache
reservation and release, RW-to-RX transitions, dynamic appends, inline-cache patches, and page
retirement no longer call Win32 memory APIs directly. Every code-write path retains an explicit
`FlushInstructionCacheRange`; on x86 it is deliberately close to a no-op because instruction fetch
is coherent with data writes, but the portable write-then-execute contract remains visible. Guest
write watches store their previous protection as `MemoryProtection`, not an OS bitmask. The default
`dynamic` backend therefore follows the same original-x86 execution path on Linux, where WSLg
`pumpit1` has produced buffer swaps and non-black pixels.

Since Task 503c fault delivery lives in `repiu::platform` too: a vectored handler on Windows and
`sigaction` on Linux produce the same `FaultKind` and `FaultEvent`, and the callback edits registers
and says whether to resume. Because `GuestCpuContext` is `CONTEXT` on Windows the callback edits the
kernel's own structure with no copy. Linux runs the handler on its own stack via `sigaltstack`, so it
survives a damaged guest stack, and sets `SA_NODEFER` because nesting is normal here.

The layer settles one convention the hosts disagree on. Windows reports `Eip` on the `int3` byte and
Linux reports the following instruction; since the engine reads the byte at `Eip` to identify a
boundary, the Linux backend rewinds to match. The rule is then the same everywhere — `Eip` names the
`int3`, and a handler that wants to continue past it advances `Eip` itself.

Since Task 503d the Linux binaries link non-PIE with their text segment lifted to 0x40000000, because
two constraints bind at once: hand-written thunks address globals directly, which in a PIE becomes a
text relocation, and a writable text segment defeats the page protection this engine detects
self-modifying code with; while plain `-no-pie` puts an i386 image at 0x08048000, inside the
0x01000000-0x09000000 range the guest's relocated image needs.

The five dispatch thunks differ only in which resolver they call, so on Linux they are one macro in
`src/platform/linux/x86/stack_bridge.inc.S` rather than five copies, written in Intel syntax so it reads
line for line against the MSVC originals. What the macro does not carry is the `fs:[4]`/`fs:[8]`
swapping each Windows thunk performs — 28 sites — because the kernel delivers a signal onto the stack
given to `sigaltstack` and asks nothing about where the interrupted stack lives. A probe that faults
while on the switched stack confirms it.

Since Task 503d-16 the trampoline's three entries into the guest are in GAS as well, in
`src/platform/linux/x86/guest_stack_switch.S`: `CallGuestEntryWithStack`, which is the stack switch
itself, and the two fault recoveries. `StackSwitchCallState`'s field offsets are defined once as
`#define`s in `include/repiu/platform/guest_stack_switch.h`, and because a `.S` goes through the C
preprocessor both assemblies and the C++ `static_assert`s read the same numbers.

Unlike the five thunks these entries carry no `fxsave`: what they call is the guest rather than a
host resolver, and the i386 System V ABI requires the x87 stack to be empty at a call boundary, so no
host state crosses the call. The segment restores in `RecoverGuestStackException` run the other way —
on Linux they are the *only* thing that restores the host's selectors, because
`StoreGuestCpuContext` deliberately does not write segment registers back.

The eleven globals that assembly reads by name live in
`src/platform/guest_stack_switch_state.cpp`, which is built on every host. That is what lets the
Linux assembly link ahead of the trampoline itself, and makes it the first engine code to **link**
on Linux rather than merely compile.

Enumerating the *whole* environment block is in the layer as `ForEachEnvironmentEntry`. Unlike 3d-9's
`ReadEnvironmentSetting`, which reads one variable, the DOS environment block copies all of them, and
the Windows backend reads the process environment with `GetEnvironmentStringsA` rather than the CRT's
`_environ` copy — the two diverge on any `SetEnvironmentVariable` after startup. The write half,
`PublishEnvironmentSetting`, must be `_putenv_s` on Windows for the mirror-image reason: this layer
reads in two places, `std::getenv` and `GetEnvironmentStringsA`, so only a function that updates both
will do.

Since Task 503d-18 threads are in `repiu::platform` as well — `CreateHostThread`,
`QueryHostThread`, `JoinHostThread`, `CloseHostThread`. The entry is `std::uint32_t(void*)`, neither
Windows' `DWORD WINAPI(LPVOID)` nor POSIX's `void*(void*)` but the call sites' own shape, and
`QueryHostThread` answers **without waiting**, because the host poll loop has Glide commands to pump
and timer ticks to deliver between questions. Reporting `running` and `exit_code` separately is
equally deliberate: `GetExitCodeThread` returns 259 for a running thread and 259 is also a legal exit
code, so a `!= STILL_ACTIVE` test reads a thread that ended with it as one that never stops; the
Windows backend splits on a zero-length wait instead. `TerminateThread` gets no Linux counterpart —
it is the last resort behind a graceful path (suspend, `RecoverToHost`, resume) that a signal
performs there, which makes the graceful path the only one.

Since Task 503d-17 the `repiu` loader **links on Linux**. Its entry point is the same
`src/host/win32/main.cpp` the Windows host uses (`src/host/loader/main.cpp` since Task 731), and the child-process relaunch divides at
`repiu::platform::RunChildProcessAndWait` — `CreateProcessA` on Windows, `posix_spawn` on Linux.
Whether Task 500's reason for that relaunch, a GPU driver claiming the address space the guest needs,
also applies to Linux is **not yet measured**. The unlimited-wait spelling moved to a neutral constant
as well, held equal to `INFINITE` by a `static_assert` beside the Windows wait.

## 웹(wasm) 빌드 / The web (wasm) build

Task 513 Stage 1부터 플랫폼 공용 코어가 **wasm32로 빌드됩니다.** `scripts/build_web_wasm.sh`가
`emcmake`로 `build/web_wasm`에 만들고, `repiu_core_probe`는 Node에서 돕니다.

**게임은 실행되지 않으며, 지금 구조로는 실행될 수 없습니다.** 실행 backend 둘이 모두 네이티브
x86 위에 서 있습니다 — `legacy`는 트랩 플래그 단일 스텝과 하드웨어 폴트 전달, `dynamic`은 RX로
매핑한 x86 코드 캐시와 INT3 sentinel입니다. wasm에는 어느 쪽도 형태가 없습니다. 그래서 Linux
이식과 달리 `src/platform/win32`의 82개 실행 엔진 소스가 Emscripten 구성에서 **제외**되고,
wasm32 빌드는 `repiu_exe`의 코어 57개 소스뿐입니다.

13개 플랫폼 헤더 중 다섯이 성립하지 않습니다 — `fault_handler`(wasm 트랩은 레지스터 컨텍스트를
싣지 않고 재개하지 않습니다), `guest_cpu_context`(호스트에 x86 레지스터가 없습니다),
`guest_stack_switch`(x86 어셈블리이고 wasm 스택은 주소지정 불가), `virtual_memory`(선형
메모리에 페이지 보호가 없습니다), `host_process`(브라우저에 자식 프로세스가 없습니다).
`src/platform/web/`가 그 다섯을 **거짓을 반환하고 이유를 한 번 찍는 stub**으로 두고, 성립하는
`safe_memory_copy`만 구현합니다. `host_environment`와 `worker_signal`은 내용이 순수 POSIX라
`src/platform/linux/`의 것을 그대로 씁니다.

**`if(UNIX)`는 Emscripten에서도 참입니다.** CMake의 다섯 가드가 모두 "리눅스 호스트"라는 뜻으로
쓰여 있었으므로 전부 `if(UNIX AND NOT EMSCRIPTEN)`이 됐습니다. 그러지 않으면 웹 빌드가 x86
어셈블리와 `-m32`를 끌어옵니다.

Stage 1 측정: 코어 소스 **수정 0건**, 엔진 자신의 미해결 심볼 **0개**, probe **9/9 통과**(성립
하지 않는 6개는 이름과 함께 `core_probe_skipped`로 출력), 포인터 폭 4바이트로 i386과 동일.
브라우저 실행까지의 나머지 네 단계는 [설계](docs/design/20260828-513-web-wasm-execution.md)에
있습니다.

Task 514 Stage 2가 더한 `repiu_instruction_census`는 **세 호스트 전부에서 빌드되는 첫
도구**입니다. parse → runtime plan → relocation → translation plan 사슬이 전부 `src/exe`와
`src/runtime`에 있어 `repiu_exe`만으로 서기 때문이고, Task 501이 `repiu_core_probe`로 세운
"같은 코드가 여러 호스트에서 같은 답을 내는가" 관례를 그대로 따릅니다. 새 디스어셈블러를 쓰지
않고 `runtime::BuildAotTranslationPlan`이 이미 만드는 계획을 셉니다 — 엔진이 실제로 번역하는
집합이 곧 Stage 3이 구현해야 하는 집합이기 때문입니다. Emscripten 구성에서만 이 타깃에
`-sNODERAWFS`가 붙습니다(Node가 호스트 파일 시스템의 EXE를 읽어야 하며, 브라우저 타깃에는
쓰지 않습니다).

Task 514 Stage 2 added `repiu_instruction_census`, the **first tool that builds on all three
hosts**. The chain parse → runtime plan → relocation → translation plan lives entirely in `src/exe`
and `src/runtime`, so it stands on `repiu_exe` alone, and it follows the convention Task 501
established with `repiu_core_probe`: does the same code give the same answer on every host. It
writes no disassembler of its own and counts the plan `runtime::BuildAotTranslationPlan` already
produces, because the set the engine actually translates is the set Stage 3 has to implement. Only
the Emscripten configuration attaches `-sNODERAWFS` to this target, so Node can read an EXE off the
host filesystem; no browser target uses it.

Since Task 513 Stage 1 the platform-neutral core **builds for wasm32.** `scripts/build_web_wasm.sh`
configures `build/web_wasm` through `emcmake`, and `repiu_core_probe` runs under Node.

**The game does not run there, and cannot with the present structure.** Both execution backends
stand on native x86: `legacy` on trap-flag single-stepping with hardware fault delivery, `dynamic`
on an RX-mapped x86 code cache with INT3 sentinels. Neither has a wasm form. So unlike the Linux
port, the 82 execution-engine sources under `src/platform/win32` are **excluded** from the
Emscripten configuration, and the wasm32 build is `repiu_exe`'s 57 core sources alone.

Five of the thirteen platform headers do not hold: `fault_handler` (a wasm trap carries no register
context and does not resume), `guest_cpu_context` (the host has no x86 registers),
`guest_stack_switch` (x86 assembly, and the wasm stack is not addressable), `virtual_memory` (linear
memory has no page protection), and `host_process` (a browser has no child processes).
`src/platform/web/` keeps those five as **stubs that return false and state their reason once**, and
implements only `safe_memory_copy`, which does hold. `host_environment` and `worker_signal` are used
from `src/platform/linux/` unchanged, being plain POSIX inside.

**`if(UNIX)` is true under Emscripten as well.** All five guards in CMake were written to mean "the
Linux host", so all five became `if(UNIX AND NOT EMSCRIPTEN)`; otherwise the web build pulls in x86
assembly and `-m32`.

Stage 1 measurements: **zero** core sources modified, **zero** unresolved symbols of the engine's
own, **9 of 9** probes passing (the six that do not hold are printed by name as
`core_probe_skipped`), and a 4-byte pointer, the same as i386. The remaining four stages toward a
browser are in the [design](docs/design/20260828-513-web-wasm-execution.md).

## 런처 / Launcher

Task 500부터 인자 없이 실행하면 런처가 열립니다. 네이티브 툴킷을 쓰지 않고 호스트가 이미
만드는 SDL3 창에 Dear ImGui(MIT)로 직접 그립니다 — 리눅스에서 쓸 만한 크로스플랫폼 네이티브
툴킷이 모두 LGPL 계열이라 프로젝트 라이선스 정책과 충돌하고, 캐비닛에는 키보드·마우스가
없어 결국 게임 창 안에서 조작할 수 있어야 하며, 같은 레이어가 나중에 인게임 OSD가 되기
때문입니다.

구성은 플랫폼 공용을 우선합니다. `launcher::RomSetCatalog`는 `std::filesystem`만으로 내장
프로필의 롬셋 가용성과 사유를 만들고(`PreparePiuChdMount`와 같은 규칙을 추출 없이 적용),
`launcher::LauncherSettings`는 `cfg/repiu.ini`를 기존 `IniDocument`로 읽고 씁니다. UI는
SDL3와 OpenGL만 쓰므로 역시 공용 트리에 있고, host `main.cpp`에는 배선만 남습니다.

설정은 새 파라미터를 subsystem마다 통과시키는 대신 **기존 소비자가 이미 읽는 환경 변수로
게시**합니다. 호출자가 이미 설정한 변수는 절대 덮어쓰지 않으므로, 벤치마크 스크립트와 진단
절차의 우선권이 그대로 유지됩니다. 인자가 있는 실행은 런처를 거치지 않고
`REPIU_LAUNCHER=0`은 인자 없는 실행에서도 런처를 건너뜁니다.

#15부터 런처 창은 크기를 바꾸고 최대화할 수 있고, 글자·여백·표 열·버튼이 창 높이에 비례합니다
(`UiScaleForHeight`, 기준 640, 최소 1 — 기본 크기에서는 이전과 같음). 표 아래 공간은 고정값 대신
직전 프레임에 잰 옵션·버튼 높이만큼 비워 어떤 배율에서도 창이 스크롤되지 않습니다. issue #45부터
런처 창도 `[Video] fullscreen`·`keep_aspect`를 따릅니다: 저장값대로 전체 화면으로 열고 Options 맨 위의
체크박스나 `Alt+Enter`로 바로 바꾸며(Alt+Enter는 ImGui로 넘기지 않아 Enter가 롬셋을 시작하지 않음),
비율 유지면 기본 창(960×640) 비율의 가장 큰 영역에 가운데로 그리고 둘레를 검게 둡니다. 배율은 그
영역의 높이를 따릅니다. 표에서 더블클릭은 롬셋 시작이라 토글로 쓰지 않습니다. 게임 중 OSD가 바꾼 값은
게임 프로세스가 저장하고, 런처 루프는 게임이 끝나면 파일을 다시 읽습니다. 폰트는 OSD와
같은 ProggyForever입니다.

Since Task 500, running with no arguments opens a launcher drawn with Dear ImGui (MIT) inside the
SDL3 window the host already creates, rather than through a native toolkit: every cross-platform
native toolkit that would serve Linux is LGPL-family and conflicts with the project's license
policy, cabinets have no keyboard or mouse so the UI must eventually live in the game window
anyway, and the same layer becomes the in-game OSD later. The structure is platform-neutral first:
`launcher::RomSetCatalog` derives each ROM set's availability and reason with `std::filesystem`
alone, applying the same rules as `PreparePiuChdMount` without extracting, and
`launcher::LauncherSettings` reads and writes `cfg/repiu.ini` through the existing `IniDocument`.
The UI touches only SDL3 and OpenGL, so it also lives in the shared tree, and `main.cpp` keeps just
the wiring. Settings reach the run by being published as the environment variables the existing
consumers already read, never overwriting one the caller set, so benchmark scripts and diagnostic
procedures keep their precedence. An argument-bearing invocation bypasses the launcher, and
`REPIU_LAUNCHER=0` skips it for argument-free automation.

Since #15 the launcher window can be resized and maximised, and its text, spacing, table columns
and buttons follow the window height (`UiScaleForHeight`, reference 640, minimum 1, so the default
size looks as before). Below the table it leaves the height the options and buttons measured on the
previous frame rather than a fixed value, so no scale makes the window scroll. Since issue #45 the
launcher window follows `[Video] fullscreen` and `keep_aspect` too: it opens fullscreen when stored and
switches at once from the checkboxes at the top of Options or `Alt+Enter` (kept from ImGui so its
Enter does not start a ROM set), and with keep-aspect on it draws in the largest area of its default
window's ratio (960×640), centred, with black around it, its scale following that area's height. A
double click in the table starts a ROM set, so it is not a toggle there. A change made in the game's
OSD is stored by the game process, and the launcher loop reads the file again when the game ends. It
draws with the same ProggyForever font as the OSD.

issue #48부터 런처는 업데이트를 확인하고 설치합니다. 플랫폼 공용 `repiu::update`(`include/repiu/update/`,
`src/update/`)가 버전 비교(`semantic_version`), 최신 릴리스 JSON과 빌드별 asset 선택(`release_info`), SHA-256,
tar.gz·zip 풀기와 경로 검사(`release_archive`, miniz의 inflate·ZIP 읽기), 교체·되돌림·`.repiu-old` 정리
(`update_install`), 백그라운드 상태 기계(`launcher_updater`)를 맡습니다. 플랫폼 계층은 이 타입을 모르고
`platform/https_download.h`(Win32 WinHTTP, Linux는 시스템 `curl`을 `posix_spawnp`로, web 스텁)와
`host_process.h`의 `HostExecutablePath`·`ReplaceProcessImage`(Linux `execv`), `host_environment.h`의
`WithdrawEnvironmentSetting`만 제공합니다. 로더는 런처 세션 시작에 지난 업데이트의 `.repiu-old`를 지우고
updater를 만들며, 런처 UI가 `install_update`로 닫히면 설치한 뒤 이 세션이 게시한 환경 변수를 거두고 다시
시작합니다(Linux는 같은 PID로 `execv`, Win32는 자식으로 실행해 기다림). 설치 버튼은 실행 파일 옆
`VERSION`이 빌드 버전과 같을 때만 나오므로 빌드 트리는 덮어쓰지 않습니다.

Since issue #48 the launcher checks for and installs updates. The platform-neutral `repiu::update`
(`include/repiu/update/`, `src/update/`) holds the version comparison (`semantic_version`), the
latest-release JSON and per-build asset choice (`release_info`), SHA-256, tar.gz and zip unpacking with path
checks (`release_archive`, through miniz's inflate and ZIP reader), replacement, rollback and `.repiu-old`
cleanup (`update_install`), and the background state machine (`launcher_updater`). The platform layer knows
none of these types and provides only `platform/https_download.h` (WinHTTP on Win32, the system `curl`
through `posix_spawnp` on Linux, a web stub), `HostExecutablePath` and `ReplaceProcessImage` (Linux `execv`)
in `host_process.h`, and `WithdrawEnvironmentSetting` in `host_environment.h`. At the start of a launcher
session the loader removes the previous update's `.repiu-old` files and creates the updater; when the
launcher UI closes with `install_update` it installs, withdraws the environment variables this session
published, and restarts (Linux `execv` keeps the PID; Win32 runs the new launcher as a child and waits). The
install button appears only when a `VERSION` next to the executable equals the build's version, so a build
tree is never overwritten.

## AOT build option toggle 관례 / AOT build-option toggle convention

Task 424는 환경 변수 하나로 기능을 켜고 끄는 관례를 플랫폼 공용
`runtime::env_toggle`로 모읍니다. 두 함수 모두 참 값은 `1`, `on`, `true` 세 가지뿐이고
대소문자 변환을 하지 않으며, 알 수 없는 값은 fail-closed OFF입니다. 오타가 조용히 ON으로
통과하면 A/B 결과를 잘못 읽게 되므로 이 성질이 중요합니다.

| 함수 | 미지정·빈 값 | 용도 |
|---|---|---|
| `ResolvePromotedToggle` | ON | A/B로 승격이 끝난 기능. 명시적 `0|off|false`만 진단용 opt-out |
| `ResolveOptInToggle` | OFF | 아직 기본값이 아닌 기능 |

Task 424는 backend 값만으로 결정되던 세 build option에 각각 toggle을 부여합니다.
셋 다 `ResolvePromotedToggle` 계열입니다.

| 환경 변수 | build option |
|---|---|
| `REPIU_AOT_DBT_RETURN_MISS_DISPATCH` | `enable_dbt_return_miss_dispatch` |
| `REPIU_AOT_DBT_DIRECT_EDGE_DISPATCH` | `enable_dbt_direct_edge_dispatch` |
| `REPIU_AOT_DBT_TIMER_SAFE_POINTS` | `enable_timer_safe_points` |

**#20 이후:** 이 세 toggle과 `REPIU_AOT_DIRECT_RETURN_TABLE`,
`REPIU_AOT_DBT_PORT_IO_DISPATCH`는 제거되어, 해당 build option은 `dynamic` backend
여부만으로 정해집니다. build option 자체는 legacy backend와 probe 대조군이 꺼진 값으로
쓰므로 남아 있습니다.

**확인됨 (Task 424):** direct-edge dispatch는 이미지에 따라 필수입니다. pumpit3의
`PIU.EXE`에는 cache 밖을 가리키는 direct edge가 10개 있어, 이 기능을 끄면 emitter가
그 edge를 표현하지 못해 `direct control-flow target is outside the cache`로 이미지
생성이 실패합니다. 그런 edge가 0개인 pumpit1은 꺼도 정상 빌드됩니다. 이 실패는
실행 전에 exit 1로 드러나므로 조용한 오동작이 아닙니다.

Task 424 collects the single-variable gating convention into the platform-neutral
`runtime::env_toggle`. Both functions accept only `1`, `on`, and `true` as true, neither folds
case, and any unrecognized value is a fail-closed OFF — a typo must never pass silently as ON,
or an A/B result is read wrong. `ResolvePromotedToggle` reads unset and empty as ON for
features already promoted by measurement, leaving explicit `0|off|false` as the diagnostic
opt-out; `ResolveOptInToggle` reads them as OFF for features that are not yet defaults.

The three options that were previously decided by the backend value alone now each carry a
promoted-style toggle: `REPIU_AOT_DBT_RETURN_MISS_DISPATCH`,
`REPIU_AOT_DBT_DIRECT_EDGE_DISPATCH`, and `REPIU_AOT_DBT_TIMER_SAFE_POINTS`. **Since #20**
these three, `REPIU_AOT_DIRECT_RETURN_TABLE` and `REPIU_AOT_DBT_PORT_IO_DISPATCH` are gone and
the options follow the `dynamic` backend alone; the options themselves remain because the
legacy backend and the probes' control images build with them off.

**Confirmed (Task 424):** direct-edge dispatch is mandatory for some images. pumpit3's
`PIU.EXE` contains ten direct edges whose targets fall outside the cache, so disabling the
feature leaves the emitter unable to represent them and image construction fails with
`direct control-flow target is outside the cache`. pumpit1, which has none, builds either way.
The failure surfaces as exit 1 before execution rather than as silent misbehaviour.

## AOT bounded jump table 번역 / AOT bounded jump-table translation

Watcom switch문 관용구(`cmp reg,imm; ja default; jmp dword ptr cs:[reg*4+disp32]`)는
planner가 cmp/ja guard로 테이블 크기(`imm+1`, 최대 61)를 확정하고 relocated image에서
전 엔트리가 image 내부임을 검증한 뒤 `kJumpTable`로 분류해 각 target을 CFG에
포함시킵니다. 방문 순서에 무관하도록 walk 후 재분류 스윕이 수렴까지 반복됩니다.
하위 byte 상태를 쓰는 compiler/library 변형은 `cmp low-r8,imm; jnbe default; and
parent-r32,0xff; jmp cs:[parent-r32*4+table]`의 def-use 관계를 추가로 검증합니다.
정확한 low-byte/parent register 관계와 `0xff` mask가 모두 일치할 때만 guard를 다음
명령으로 전달하며 high-byte register, 다른 mask/register 또는 정규화 생략은 거부합니다.
emitter는 `jmp [reg*4+native_table]` + INT3 fallback + 인라인 포인터 테이블을
방출하고, Win32 배치·동적 추가의 RW 윈도우에서 절대 주소를 기록합니다.
미번역 target 엔트리는 INT3 fallback을 가리켜 dispatcher로 fail-closed합니다.

The planner recognizes the Watcom switch idiom, derives the table bound from the
cmp/ja guard (up to 61 entries), validates every relocated table entry as
in-image, classifies the branch `kJumpTable`, and enqueues each target into the
CFG, with a post-walk reclassification sweep making the result independent of
block visit order. Compiler/library variants using byte states may use `cmp
low-r8,imm; jnbe default; and parent-r32,0xff; jmp
cs:[parent-r32*4+table]`; the planner propagates the guard only when the exact
low-byte/parent relationship and `0xff` mask are verified. High-byte registers,
different masks/registers, and omitted normalization fail closed. The emitter
produces `jmp [reg*4+native_table]` with an INT3
fallback and an inline pointer table; Win32 placement and dynamic append resolve
absolute addresses in their RW windows, and untranslated entries fail closed to
the dispatcher.

```mermaid
flowchart LR
    G["cmp reg,imm + ja"] --> J["jmp cs:[reg*4+disp32]"]
    B["cmp low8,imm + ja"] --> Z["and parent32,0xff"]
    Z --> J
    J -->|"검증 통과"| N["native table jmp<br/>(kJumpTable)"]
    J -->|"검증 실패"| X["INT3 dispatcher exit"]
    N -->|"미번역 entry"| X
```

## AOT self-modifying page 세대

AOT placement는 각 address-map entry의 guest page, 세대, active 상태와 영구적인
cache-to-guest provenance를 별도로 보관합니다. 번역된 명령 범위와 겹치는 guest
write가 확인되면 serialized worker가 그 page의 active entry 첫 바이트를 `INT3`로
바꾸고 active guest lookup에서 제외합니다. 기존 cache 주소는 삭제하지 않으므로
이미 연결된 direct edge나 inline cache가 도달해도 guest 주소를 복원할 수 있습니다.
같은 writable 구간에서 retire page를 guest target으로 학습한 모든 inline-cache
guard를 초기 `E9 → miss tail` 형태로 복원해(Task 245, 설계 238), 학습된 hit이
retired entry의 `INT3`로 영구 trap하는 대신 다음 전송이 dispatcher의 정상 miss
재패치 프로토콜을 타게 합니다.

다른 page에서 patch한 retired page로 다음에 진입할 때 live guest byte를 snapshot해
새 세대를 발행합니다. 길이가 5바이트 이상인 오래된 entry는 최신 entry로 가는
`E9 rel32`로 재연결하고, 짧은 entry는 provenance trap으로 남깁니다. 같은 page의
self-modification은 해당 page만 legacy quarantine하고, 번역·발행 실패는 아래
세대 실패 정책을 따릅니다. 정상 경로는 inactive map이 없으면 추가 탐색을 하지
않으며, 재연결도 inactive index만 순회합니다.

**세대 실패의 범위 (Task 415).** 재번역 실패는 예전에 그 entry의 guest page 전체를
영구 quarantine했고, 그러면 같은 page의 다른 routine이 전부 single-step으로
떨어졌습니다. 실패한 것은 entry 하나이므로 지금은 **실패한 guest 주소만** 억제
집합(용량 256)에 넣고 다시 시도하지 않습니다. 재시도 storm을 막는다는 quarantine의
성질은 유지하면서 나머지 page는 계속 cache에서 돕니다.
억제 집합이 가득 차면 예전 page 단위 격리로 돌아갑니다(예전 동작을 되살리던
스위치는 issue #22에서 삭제). 정책은 `aot_generation_failure_policy.h`가 노출하는
counter(실패 주소 수, 건너뛴 시도, page 격리 횟수, 걸친 활성화 횟수)로 보고합니다.

**page 경계를 걸친 요청 항목 (Task 417).** 그 세대 실패의 근인이 여기였습니다.
`CanActivateAotAddressMapEntry`는 entry가 걸친 page가 retired면 활성화를
거부하는데 **요청 page만 예외**였으므로, 이웃 page가 retired된 뒤에는 경계를 걸친
요청 entry가 다시는 활성화되지 못하고 실행이 arena로 떨어졌습니다. append 루프는
이제 **요청 항목 하나에 한해**, quarantined page를 걸치지 않는 한 활성으로 둡니다.
지금 막 현재 guest byte로 번역한 image이고 `RegisterAddressMapPages`가 걸친 **모든**
page에 등록하므로 이후 어느 page에 써도 같은 entry가 retire됩니다. 나머지 entry는
규칙 그대로입니다(예전 거부 규칙을 되살리던 스위치는 issue #22에서 삭제).

`aot_page_coherence_win32`는 translated instruction이 있는 guest page를
`PAGE_EXECUTE_READ`로 감시합니다. native guest/cache store의 write fault에서는
Zydis로 store 폭을 계산하고 관련 page만 한 명령 동안
`PAGE_EXECUTE_READWRITE`로 전환한 뒤 Trap Flag 완료 시 다시 RX로 복원합니다.
`WriteGuest*` HLE helper는 write와 보호 복원이 성공한 뒤 같은 retirement 정책에
통지합니다. code cache 자체는 worker만 RX→RW→RX로 바꾸고 수정 후 항상
`FlushInstructionCache`를 호출합니다.

```mermaid
stateDiagram-v2
    [*] --> Active
    Active --> Retired: overlapping guest write
    Retired --> Translating: next page entry
    Translating --> Active: publish generation N+1
    Translating --> Suppressed: 세대 실패 → 실패 주소만 (Task 415)
    Suppressed --> Quarantined: 억제 집합 포화
    Active --> Quarantined: same-page self modification
    note right of Suppressed
        걸친 요청 항목은 이제 활성화되므로
        이 전이 자체가 드물어집니다 (Task 417)
    end note
```

## Glide host-thread rendezvous의 스핀 대기

Glide 명령은 GL 컨텍스트를 소유한 host thread에서 실행되므로, 게스트 스레드는
`InvokeOnHostThread`에서 명령을 publish하고 완료를 기다립니다. Task 418이 pumpit3에서
그 대기가 **gate 시간의 65.9%**(wake 35.2% + complete 30.5%)임을 측정했습니다. 호출당
왕복 고정비가 약 70,000 cycle인데 호출당 GL 작업은 34,745 cycle이라, **작은 호출이
왕복에 묻히는** 구조였습니다. 호출이 훨씬 크고 드문 pumpit1은 같은 지점이 7.6%입니다.

Task 419가 대기 세 곳(publish 전 pending 대기, 완료 대기, host의 pending 대기)을
**짧은 스핀 후 조건변수 폴백**으로 바꿨습니다. 스핀은 `_mm_pause`로 돌며 두 플래그의
`std::atomic<bool>` 미러를 읽고, 예산은 `REPIU_GLIDE_RENDEZVOUS_SPIN_US`(기본 20 µs,
`0`이면 예전 동작)입니다.

**원자 미러는 힌트일 뿐입니다.** 스핀이 조건을 관측해도 반드시 뮤텍스를 잡고 기존
조건변수 술어로 재확인하며, 술어 자체는 바뀌지 않습니다. 이 규칙이 lost wakeup을
막습니다. 미러는 원래 플래그와 **같은 임계구역에서** 갱신되므로 락 밖에서 읽어도
순서가 뒤집히지 않습니다.

측정(60초 A/B, pumpit3): `wake+complete` 65.4~66.3% → **5.3~12.9%**, 프레임
2,399 → **3,063 중앙값(+27.7%)**. rendezvous 처리량이 +20%이고 프레임당 호출 수는
그대로이므로, gate가 짧아진 것이 아니라 **대기가 작업으로 바뀐 것**입니다.
스핀 hit율은 게스트측 99.5% 이상, host측 약 94%입니다.

플랫폼 공용 번역 계획은 HLE가 소유한 guest 범위를 제외 범위로 받을 수 있습니다.
LINEXE/Glide 합성 gate는 복사된 `UD2`로 실행하지 않고 cache sentinel에서 원본
guest 주소로 나온 뒤 기존 HLE dispatcher가 처리합니다.

x64에서 guest 상태가 어디 있는지는 `linux_x64_guest_registers.h`가 결정으로 들고
있습니다(Task 558·559). guest GPR *n*은 host GPR *n*이고(항등이며, `kIdenticalBytes`가
강제합니다), guest `ESP`는 `R15D`, emitter scratch는 `R14D`입니다 — 확장 레지스터인 이유는
**32비트 인코딩이 그것들을 이름 부를 수 없기** 때문이고, callee-saved라 SysV가 보존합니다.
stack 명령은 `R15D`를 쓰는 시퀀스로 낮춰지며 `ESP` 조정은 항상 `LEA`입니다 — guest
`PUSH`·`POP`이 flag를 바꾸지 않기 때문입니다.

Where guest state lives on x64 is held as a decision in
`linux_x64_guest_registers.h` (Tasks 558 and 559). Guest GPR *n* is host GPR *n* -- the
identity, forced by `kIdenticalBytes` -- while guest `ESP` is `R15D` and the emitter's
scratch is `R14D`; they are extended registers because **a 32-bit encoding cannot name
them**, and callee-saved so SysV preserves them. Stack instructions lower to sequences
using `R15D`, and the `ESP` adjustment is always a `LEA`, because guest `PUSH` and `POP`
change no flags.

Task 606은 정확한 `66 50..5F` register PUSH/POP을 16비트 MOV와 ESP ±2로
변환합니다. 상위 register 비트와 인접 반환주소를 보존하며 SP는 R14 scratch를
통해 감소 전 저장 및 증가 후 하위 word 반영을 처리합니다.

Task 606 lowers exact `66 50..5F` register PUSH/POP with word MOV and ESP ±2.
It preserves upper register bits and adjacent return addresses. SP uses R14
scratch for the pre-decrement value and post-increment low-word update.

Task 644부터 x64 `aot_legacy_fallback`에서 원본 바이트를 single-step할 때도
한 바이트 `50h..5Fh` register PUSH/POP은 `instruction_emulation`의 공용 HLE가
guest `ESP`와 guest memory에 적용합니다. long mode에서 같은 바이트가 host
`RSP`를 사용해 의미가 조용히 달라지는 것을 막기 위한 경계이며, i386 직접 실행과
AOT cache 실행에는 적용하지 않습니다. `PUSH ESP`는 감소 전 값을 저장하고
`POP ESP`는 증가 뒤 레지스터 기록이 최종 ESP를 정하는 32-bit x86 순서를 보존합니다.

From Task 644, while x64 `aot_legacy_fallback` single-steps original bytes, the
one-byte `50h..5Fh` register PUSH/POP forms are applied to guest `ESP` and guest
memory by shared `instruction_emulation` HLE. This boundary prevents the same
bytes from silently using host `RSP` in long mode and does not affect i386
direct execution or AOT-cache execution. It preserves 32-bit x86 ordering:
`PUSH ESP` stores the pre-decrement value, while `POP ESP` takes its final ESP
from the register write after the conceptual increment.

Task 646부터 x64 `aot_legacy_fallback`에서 첫 스택 HLE가 성공하면 뒤따르는
지원 대상 register/segment push/pop도 같은 예외 안에서 최대 16개까지 연속
처리합니다. 각 반복은 guest code 범위와 EIP 전진을 확인하며, 비스택 명령이나
거부된 접근에서 즉시 기존 실행 경로로 돌아갑니다. 이 경계는 유효한 x64 명령인
`PUSH FS/GS`가 다음 fault 전에 host `RSP`를 바꾸는 틈을 막습니다.

From Task 646, after the first successful stack HLE in x64
`aot_legacy_fallback`, up to 16 following supported register/segment push/pop
instructions are consumed in the same exception. Every iteration validates the
guest code range and EIP progress, and stops immediately at a non-stack
instruction or rejected access. This closes the gap in which valid long-mode
`PUSH FS/GS` instructions could change host `RSP` before another fault.

emitter는 long mode 호스트를 위한 방출 모드를 하나 갖습니다(Task 553).
`AotCodeCacheBuildOptions::enable_long_mode_emission`이 켜지면 `kCopy`마다
`ClassifyLongModeBytes`(Task 550)로 판정하고 `LowerLongModeBytes`(Task 552)로 낮추며,
낮출 수 없는 것과 x64 전용 slot이 아직 없는 kind는 기존 `INT3` 경계로 갑니다 — i386
slot은 손으로 쓴 32비트 시퀀스이고 long mode가 그 중 여럿을 조용히 다르게 읽기
때문입니다. direct branch/call/return, segment override, guarded segment load는 후속 task가
각각 전용 x64 slot과 실행 probe를 만든 뒤 열었습니다. 기본값은 `false`이므로 i386 방출은 이 판단을 지나가지 않습니다.
호스트 매크로가 아니라 option인 이유는 방출이 계산이어서 같은 plan에 대한 답이 모든
호스트에서 같아야 하고, 그래야 Windows에서 그 답을 볼 수 있기 때문입니다.

The emitter has one emission mode for a long-mode host (Task 553). With
`AotCodeCacheBuildOptions::enable_long_mode_emission` on, every `kCopy` is judged by
`ClassifyLongModeBytes` (Task 550) and lowered by `LowerLongModeBytes` (Task 552);
anything that cannot be lowered, and every kind without a dedicated x64 slot, reaches the
existing `INT3` boundary, because the i386 slots are hand-written 32-bit sequences that
long mode reads differently without raising. Later tasks opened direct branches, calls,
returns, segment overrides, and guarded segment loads only after adding dedicated x64
slots and execution probes. The default is `false`, so i386 emission
never enters that judgement. It is an option rather than a host macro because emission is
computation: the answer for a given plan must be the same on every host, which is what
keeps it observable on Windows.

Task 639부터 long-mode에서 cache 밖을 향해 중립화된 `kBlockFallthrough`도 fixup
metadata로 복구됩니다. 이 slot은 address-map entry가 아니므로 runtime의
`FindAotBlockFallthroughTarget`이 `INT3` 위치와 미해결 fixup의 rel32 위치를 대조하여
guest target을 반환합니다. execution engine은 그 target을 기존 AOT boundary의 TF
원본 실행 경로로 전달합니다. 잘못된 주소, 이미 해결된 fixup, 다른 direct-edge kind는
거절되며 sentinel provenance는 `kOtherPlannerFixup`입니다.

Since Task 639, long mode also recovers a neutralized out-of-cache
`kBlockFallthrough` through fixup metadata. Because this slot has no address-map
entry, runtime `FindAotBlockFallthroughTarget` matches the `INT3` position against
the unresolved fixup's rel32 position and returns its guest target. The execution
engine feeds that target into the existing AOT-boundary TF original-execution
path. Wrong addresses, resolved fixups, and other direct-edge kinds are rejected;
the sentinel provenance is `kOtherPlannerFixup`.

Task 569부터 `kGuardedSegmentLoad`도 x64 전용 slot을 갖습니다. x64는 guest
selector를 host `ES`/`DS`/`FS`/`GS`에 설치하지 않으므로 i386 slot의 물리
selector 비교를 옮기지 않습니다. 대신 source GPR의 하위 16비트가 shadow
selector와 같을 때만 원본 `MOV Sreg,r16`을 의미상 no-op으로 통과시키고, 다르면
flags를 복원한 뒤 INT3 HLE 경계로 갑니다. 비교 전후 flags 저장·복원은 Task 559의
stack lowering을 재사용하고 shadow access는 하위 4 GiB absolute SIB 형식입니다.

guarded-load patch의 바이트 쓰기는 segment override와 같은 runtime/engine 경계를
따릅니다. emitter가 자기 prologue를 site에 기록하고
`PatchAotGuardedSegmentLoadSites`가 writable bytes에 shadow 주소와 i386에서만 있는
counter 주소를 씁니다. engine은 page protection과 instruction-cache flush만
담당합니다. 따라서 patcher는 x64 slot을 i386의 `9C` 시작이라고 가정하지 않습니다.

Since Task 569, `kGuardedSegmentLoad` also has an x64-specific slot. An x64 host
does not install guest selectors into host `ES`/`DS`/`FS`/`GS`, so the i386
slot's physical-selector comparison is not carried over. Instead, the original
`MOV Sreg,r16` passes as a semantic no-op only when the source GPR's low 16 bits
equal the shadow selector. A mismatch restores flags and reaches the INT3 HLE
boundary. Task 559's stack lowering saves and restores flags around the
comparison, and the shadow access uses the below-4-GiB absolute SIB form.

Guarded-load byte patching follows the same runtime/engine seam as segment
overrides. The emitter records its own prologue in the site,
`PatchAotGuardedSegmentLoadSites` writes the shadow address and the i386-only
counter addresses into writable bytes, and the engine owns only page
protection and instruction-cache flushing. The patcher therefore never assumes
that an x64 slot begins with i386's `9C` layout.

Task 570부터 x64 segment-override slot은 기존 absolute `disp32` 외에 비-SIB
`base+disp8`도 허용합니다. `disp8`을 부호 확장하고 ModRM을 `mod=10`으로 바꿔
`disp32`로 넓힌 뒤, 기존 site의 `original_displacement + live segment base` patch
계약을 그대로 적용합니다. access 앞의 `0x67`은 long mode에서도 guest의 32비트
base register 주소 계산을 유지합니다. guest `ESP`는 `R15D`에 있으므로 다른 operand가
`ESP`를 가리키는 명령과 SIB 및 다른 displacement 형식은 계속 fail-closed합니다.

Since Task 570, the x64 segment-override slot admits a non-SIB base-plus-disp8
form in addition to the existing absolute disp32 form. It sign-extends disp8,
forces ModRM to `mod=10`, widens the field to disp32, and applies the existing
site contract of `original displacement + live segment base`. The access's
`0x67` retains the guest's 32-bit base-register address calculation in long
mode. Instructions naming guest `ESP` in another operand remain fail-closed
because it lives in `R15D`; SIB and the other displacement forms remain closed
as well.

Task 636부터 x64 segment-override slot은 GS와 비-SIB `mod=00`
base/no-displacement 형식도 허용합니다. GS prefix는 emitted access에서 제거되고
live GS base가 새 disp32에 접히므로 host GS를 읽거나 변경하지 않습니다. 변위가
없는 형식의 `original_displacement`는 0이며 원본 suffix는 ModRM 바로 뒤에서
복사합니다. 따라서 trailing immediate가 prefix나 원본 명령과 중복되지 않습니다.
FS, SIB, base+disp32는 계속 fail-closed합니다.

Since Task 636, the x64 segment-override slot also admits GS and the non-SIB
`mod=00` base/no-displacement form. The emitted access drops the GS prefix and
folds the live GS base into a new disp32, so host GS is neither read nor
modified. A no-displacement site records zero as `original_displacement` and
copies the original suffix from immediately after ModRM, preventing a trailing
immediate from being confused with the prefix or duplicated instruction bytes.
FS, SIB, and base+disp32 remain fail-closed.

Task 637부터 HLE가 원본 명령을 완전히 처리하고 EIP를 전진시킨 뒤의 즉시 cache
재진입은 `aot_reentry_pending`뿐 아니라 `aot_legacy_fallback`에서도 허용됩니다.
미매핑 동적 target에서 시작한 fallback이 나중에 알려진 cache 주소에 도달하면 AOT로
복귀한다는 초기 backend 계약을 복원한 것입니다. 두 상태는 자격만 부여하며,
segment-write, guest arena, quarantine, 정확한 cache hit와 span preflight 검사는 기존과
동일하게 적용됩니다. cache miss의 post-HLE 동적 번역 옵트인은 issue #22에서
삭제되었습니다.

Since Task 637, immediate cache re-entry after HLE fully handles an original
instruction and advances EIP is eligible from either `aot_reentry_pending` or
`aot_legacy_fallback`. This restores the original backend contract that a
fallback begun at an unmapped dynamic target returns to AOT after reaching a
known cache address. These states grant eligibility only; the existing
segment-write, guest-arena, quarantine, exact-cache-hit, and span-preflight
gates still apply. Post-HLE translation on a cache miss remains opt-in.

Task 571부터 `kGuardedSegmentPop`도 x64 전용 slot을 갖습니다. i386 slot은 host의
물리 segment selector를 비교하지만 x64는 guest selector를 host `DS`에 설치하지
않으므로, x64 slot은 guest stack top의 하위 16비트를 shadow selector와 비교하고
일치하면 `ESP += 4`만 수행합니다. 비교 대상 stack word는 `[r15]`가 아니라
`[r15+4]`에 있습니다 — Task 559의 lowered `PUSHFD`가 guest stack에 쓰기 때문에
flags 저장 뒤 guest ESP가 진입 값보다 4 작기 때문입니다.

Since Task 571, `kGuardedSegmentPop` has an x64-specific slot too. The i386 slot
compares the host's physical segment selector, but an x64 host never installs a
guest selector into host `DS`, so the x64 slot compares the low 16 bits of the
guest stack top against the shadow selector and performs only `ESP += 4` on a
match. The stack word to compare sits at `[r15+4]`, not `[r15]`: Task 559's
lowered `PUSHFD` writes to the guest stack, so after the flags save guest ESP is
four below its entry value.

Task 572부터 `kAbsoluteToSib` lowering은 disp32 뒤에 immediate가 오는 형식도
허용합니다. immediate의 값은 인코딩 안의 위치에 의존하지 않으므로 SIB 삽입으로
한 바이트 밀려도 의미가 보존됩니다. 허용 여부는 length 산술이 아니라 Zydis raw
정보로 판단합니다 — `disp.size == 32`, `disp.offset == modrm_offset + 1`,
displacement 끝과 명령 끝 사이의 바이트가 전부 immediate로 설명될 것, `is_relative`
immediate가 없을 것. `disp.size == 32` 조건은 `0x67` prefix가 붙은 16-bit
addressing의 `mod=00 rm=101`(`[DI]`, displacement 없음)을 계속 거절합니다.

Since Task 572, the `kAbsoluteToSib` lowering also admits forms with an
immediate after the disp32. An immediate's value does not depend on where it
sits in the encoding, so pushing it one byte later with the SIB preserves the
meaning. Admission is decided from Zydis raw information rather than length
arithmetic: `disp.size == 32`, `disp.offset == modrm_offset + 1`, every byte
between the end of the displacement and the end of the instruction accounted for
as an immediate, and no `is_relative` immediate. The `disp.size == 32` condition
keeps refusing 16-bit addressing's `mod=00 rm=101` under a `0x67` prefix, which
is `[DI]` and carries no displacement at all.

Task 573부터 `kIndirectExit`도 x64 전용 slot을 갖습니다. near `FF /2`의 memory
형식만 열려 있고, 세 조각의 결합입니다 — target을 `R14D`로 읽고, return 주소를
guest stack에 push하고, Task 562의 thunk로 넘깁니다. 그 thunk 계약은 "`R14D`에
guest 주소가 있으니 그리로 가라"이므로 return에 고유하지 않습니다. target을 읽는
명령은 guest bytes에서 opcode `FF`를 `8B`로, ModRM `reg`를 `010`에서 `110`으로 바꾼
32-bit 명령을 합성해 `LowerLongModeBytes`에 넘겨 얻고, `REX.R` 한 바이트로 `ESI`를
`R14D`로 바꿉니다. **순서는 load 다음 push이며 정확성 요건입니다** — x86은 target을
먼저 계산하고, 그래서 push sequence가 `R14D`를 건드리지 않아야 합니다. register
형식, `FF /4` jump, guest `ESP`를 가리키는 operand는 계속 거절합니다.

Since Task 573, `kIndirectExit` has an x64-specific slot as well. Only the
memory forms of a near `FF /2` are open, and it is a composition of three
pieces: read the target into `R14D`, push the return address onto the guest
stack, and hand over to Task 562's thunk. That thunk's contract -- "`R14D` holds
a guest address, go there" -- is not return-specific. The instruction reading
the target is obtained by synthesising a 32-bit instruction from the guest bytes
(opcode `FF` to `8B`, ModRM `reg` from `010` to `110`), passing it through
`LowerLongModeBytes`, and turning `ESI` into `R14D` with one `REX.R`. **The
order is load then push, and it is a correctness requirement**: x86 computes the
target first, which in turn requires that the push sequence leave `R14D` alone.
The register form, `FF /4` jumps, and operands naming guest `ESP` remain
refused.

Task 574부터 `kStackPointerToR15` 재인코딩은 두 바이트 opcode도 처리합니다.
opcode가 시작하는 자리를 `modrm.offset - 1`이 아니라 `raw.prefix_count`에서
읽으므로 `0F` map에서도 `REX`가 opcode 전체 앞에 오고, 필수 prefix가 있으면 그
뒤에 옵니다. map별 ModRM 위치(`DEFAULT`는 `prefix_count + 1`, `0F`는 `+ 2`)를
단언하고 그 밖의 map은 거절합니다. `ESP`를 `R15`로 바꾸는 데는 `REX.B`와 함께
SIB base 필드를 `111`로 바꾸는 것이 둘 다 필요합니다 — 한쪽만으로는 host `RSP`를
계속 가리킵니다.

Since Task 574, the `kStackPointerToR15` re-encoding also handles two-byte
opcodes. It reads where the opcode begins from `raw.prefix_count` rather than
`modrm.offset - 1`, so in the `0F` map the `REX` lands before the whole opcode
and after any mandatory prefix. The per-map ModRM position is asserted
(`prefix_count + 1` for `DEFAULT`, `+ 2` for `0F`) and other maps are refused.
Turning `ESP` into `R15` takes both the `REX.B` bit and the SIB base field
becoming `111`; either alone still addresses through host `RSP`.

### x64가 게스트를 실행하는 경로 (Tasks 575~578)

x86-64 host는 게스트 자신의 바이트를 실행하지 않습니다. `IsDirectX86ExecutionSupported`
는 x64에서 계속 false이고 그것이 옳습니다 — long mode는 게스트의 여러 인코딩을 다르게
읽습니다(Task 550). 대신 `IsCodeCacheEntrySupported`가 참이고, 진입은 stack switch·
direct 옆의 **세 번째 경로**입니다.

* `enable_long_mode_emission`은 `HostRequiresLongModeEmission()`이 정합니다(Task 576).
  toggle이 아닙니다 — x86-64에서 이 플래그가 꺼진 이미지는 다른 선택지가 아니라 틀린
  이미지입니다. 플래그는 image → placement → dynamic append로 흐르므로, 정적 캐시와
  나중에 붙는 block이 다른 종류가 될 수 없습니다.
* `guest_entry_x64.S`가 guest GPR을 host 동번호 레지스터에, guest ESP를 `R15D`에 싣고
  placement의 `entry_address`로 들어갑니다(Task 578).
* resolver는 `FindAotCacheAddress` 위의 어댑터이고, 0을 답하면 Task 562의 thunk가
  `INT3`을 놓습니다. return continuation은 공용 `ResolveAotTransferTarget`으로
  기존 cache를 찾거나 dynamic append를 요청하며, append가 coverage 검증에서 거절되면
  같은 fail-closed INT3 계약을 유지합니다.
* Task 562의 thunk는 guest `ret`과 간접 call/jump를 모두 받으므로, 그 명령들처럼
  **guest EFLAGS를 그대로 넘겨야 합니다.** 진입 시 frame의 `eflags`에 저장한 값을
  `jmp` 직전에 `popfq`로 복원합니다(Task 734). 복원이 없던 동안에는 resolver 뒤의
  `test r10, r10`이 CF=0을 남겨, CF로 결과를 돌려주는 Watcom `sin`/`cos` helper의
  호출자가 `fsin`을 끝없이 재시도했습니다(pumpit2a 곡 선택 정지).
* fault 경로에서 `GuestCpuContext::Esp`는 `R15`에 연결되고 host `RSP`는 기록되지
  않습니다(Task 577). `Eip`는 `RIP`의 하위 32비트이며, 엔진이 그것을 cache 주소로
  취급해 address map으로 변환할 때 cache가 4 GiB 아래에 있으므로 잘림이 없습니다.
* 미처리 폴트는 `[repiu-fault] unhandled signal=… rip=… eip=… access=…`를 씁니다.
  `rip`는 kernel context의 full host RIP이고 `eip`는 engine의 32-bit 투영입니다.
  Windows의 unhandled-exception filter에 해당하는 것이 Linux에 없었습니다.

**알려진 공백**: cache 안에서 일어난 access-violation 폴트를 guest 주소로 되돌리는
경로가 없습니다. `HandleAotReentry`는 `kBreakpoint`일 때만 그 변환을 하고, `Eip`에서
바이트를 읽는 HLE handler들은 `Eip`가 게스트 arena 안일 것을 요구합니다. 자세한 것은
`docs/analysis/linux-port-frontier.md` 3.26·3.27.

### How x64 runs a guest (Tasks 575–578)

An x86-64 host does not execute the guest's own bytes.
`IsDirectX86ExecutionSupported` stays false there and that is correct -- long mode
reads several of the guest's encodings differently (Task 550). What is true
instead is `IsCodeCacheEntrySupported`, and the entry is a **third path** beside
the stack switch and the direct call.

* `enable_long_mode_emission` is decided by `HostRequiresLongModeEmission()`
  (Task 576). Not a toggle: on x86-64 an image with the flag off is a wrong image
  rather than a different one. The flag flows image → placement → dynamic append,
  so the static cache and later-appended blocks cannot become different kinds.
* `guest_entry_x64.S` loads guest GPRs into the host registers of the same number
  and guest ESP into `R15D`, then enters at the placement's `entry_address`
  (Task 578).
* The resolver uses the shared `ResolveAotTransferTarget` policy for return
  continuations, finding an existing cache entry or requesting a dynamic append.
  A coverage-rejected append still answers zero, making Task 562's thunk plant
  the same fail-closed `INT3`.
* Task 562's thunk takes every guest `ret` and indirect call/jump, so like those
  instructions it **must hand guest EFLAGS through unchanged**. The value saved
  into the frame's `eflags` on entry is restored with `popfq` right before the
  `jmp` (Task 734). Without that restore, the `test r10, r10` after the resolver
  left CF=0, and the caller of the Watcom `sin`/`cos` helper, which answers in CF,
  retried `fsin` forever (the pumpit2a song-select freeze).
* In the fault path `GuestCpuContext::Esp` binds to `R15` and host `RSP` is never
  written (Task 577). `Eip` is the low 32 bits of `RIP`; when the engine treats
  it as a cache address and translates it through the address map, the cache
  sits below 4 GiB so nothing truncates.
* An unhandled fault writes `[repiu-fault] unhandled signal=… rip=… eip=… access=…`.
  `rip` is the full host RIP from the kernel context; `eip` is the engine's
  32-bit projection. Linux had no equivalent of Windows's unhandled-exception
  filter.

Task 647 adds bounded recovery provenance to `ThreadContext`. Immediately
before a fault callback or shutdown interrupt redirects execution through
`RecoverToHost`, it records the source EIP, whether that source belongs to the
guest image or the AOT cache, and the recovery path. `REPIU_FAULT_EXIT_TRACE`
prints these as `recovery_source_eip`, `recovery_source`, and `recovery_path`
next to the current exit EIP, so a later x64 `UD2` at the recovery destination
can still be attributed to the original executable location. This is telemetry
only: it does not alter execution or recovery policy, and the default behavior
is unchanged when the environment toggle is unset.

Task 648 adds a separate `LinuxX64TransferFailureProvenance` record to the
same `ThreadContext`. When the Linux x64 return resolver cannot resolve a
target, it stores the producer kind/site from frame `status`, the
`guest_source` target, guest `ESP` after the transfer, and the failure reason
in a fixed-size record. Target `0` is retained as a valid observation rather
than discarded as an invalid sentinel. The previous record is cleared at the
next resolver entry, so the final fault trace links only the most recent
failure. `REPIU_FAULT_EXIT_TRACE` prints `x64_transfer_valid`,
`x64_transfer_producer_eip`, `x64_transfer_target_eip`,
`x64_transfer_guest_esp`, `x64_transfer_kind`, and `x64_transfer_failure`.
The resolver's `return 0`, the unresolved thunk's `INT3/UD2`, guest state, and
AOT policy remain unchanged.

Task 647은 `ThreadContext`에 bounded 복구 provenance를 추가합니다. fault
callback 또는 shutdown interrupt가 `RecoverToHost`를 통해 실행을 재지정하기
직전에 원인 EIP, 해당 위치가 guest image인지 AOT cache인지, 복구 경로를
기록합니다. `REPIU_FAULT_EXIT_TRACE`는 현재 exit EIP 옆에
`recovery_source_eip`, `recovery_source`, `recovery_path`를 출력하므로, x64
복구 목적지의 후속 `UD2`도 원래 실행 위치에 연결할 수 있습니다. 이는
telemetry 전용이며 실행·복구 정책을 바꾸지 않고, 환경 변수가 꺼져 있으면
기존 동작을 그대로 유지합니다.

Task 648은 별도의 `LinuxX64TransferFailureProvenance`를 같은
`ThreadContext`에 둡니다. Linux x64 return resolver가 target을 해석하지
못하면 frame `status`의 producer kind/site, `guest_source` target,
transfer 후 `guest.esp`, 실패 이유를 고정 크기 record에 저장합니다. target
`0`은 invalid sentinel로 버리지 않고 유효한 관측값으로 보존합니다. 다음
resolver 진입 때 이전 record를 지우므로 최종 fault trace에는 가장 최근
실패만 연결됩니다. `REPIU_FAULT_EXIT_TRACE`는
`x64_transfer_valid`, `x64_transfer_producer_eip`,
`x64_transfer_target_eip`, `x64_transfer_guest_esp`,
`x64_transfer_kind`, `x64_transfer_failure`를 출력합니다. resolver의
`return 0`, unresolved thunk의 `INT3/UD2`, guest 상태와 AOT policy는
변경하지 않습니다.

**Known gap**: nothing translates an access-violation fault raised inside the
cache back to a guest address. `HandleAotReentry` does that only for
`kBreakpoint`, and the HLE handlers that read bytes at `Eip` require `Eip` to be
inside the guest arena. See `docs/analysis/linux-port-frontier.md` 3.26 and 3.27.

Task 607은 DOS 종료를 받은 cache 실행을 위한 Linux x64 전용 종료 경계를
추가합니다. `FaultEvent::host_resume_address`는 선택적인 full-width 값이며,
일반 `GuestCpuContext` 되쓰기는 여전히 32비트 guest register를 담당하고
변경되지 않습니다. `RecoverFromHleExit`가 `process_exit`를 세운 뒤 Linux
signal callback은 single-step·direction flag를 지우고 native RIP를
`RepiuLinuxX64GuestExit`로 기록합니다. 이 assembly stub은 하나의 `ret`만
수행하여 `RepiuLinuxX64GuestEntry`가 SysV stack에 둔 host return address를
소비하고, 기존 entry bridge가 상태를 기록하고 저장 register를 복원하게
합니다. 이 경로는 의도된 HLE 종료에만 적용되며, 무관한 cache fault는 계속
fail-closed 미처리 fault 경로를 따릅니다.

Task 607 adds a separate Linux x64 exit edge for a cache run that has received
an intentional DOS termination. `FaultEvent::host_resume_address` is optional
and full-width; the ordinary `GuestCpuContext` write-back still owns the
32-bit guest registers and remains unchanged. After `RecoverFromHleExit` marks
`process_exit`, the Linux signal callback clears single-step and direction flags
and writes the native RIP of `RepiuLinuxX64GuestExit`. That assembly stub is a
single `ret`: it consumes the host return address that
`RepiuLinuxX64GuestEntry` placed on the SysV stack, then lets the existing entry
bridge write back state and unwind its saved registers. The path is restricted
to the intentional HLE exit; an unrelated cache fault still follows the
fail-closed unhandled-fault path.

파일 책임은 다음과 같습니다.

* `aot_code_cache_reservation`: code cache를 어디에 놓아도 되는가. cache 주소는
  host pointer인데 placement가 32비트 필드에 담으므로, 64비트 host에서는 하위 4 GiB
  후보를 차례로 요청하고 32비트 host에서는 예전처럼 hint 없이 요청한다(Task 554).
  *Where a code cache may live. A cache address is a host pointer kept in a 32-bit
  placement field, so a 64-bit host asks for below-4-GiB candidates in turn while a
  32-bit host asks with no hint exactly as before (Task 554).*
* `aot_page_coherence_win32`: page index/state, overlap query, retirement,
  write-watch 보호 전환
* `aot_code_cache_win32`: placement, dynamic append, generation publication,
  stale-entry relink 조율
* `aot_inline_cache_site_index`: inline-cache miss site exact 조회 cache
* `aot_return_dispatch_site_index`: return miss thunk의 dispatch site exact 조회 cache
* `aot_return_patch_policy`: DBT return site별 target 다양성과 megamorphic patch 우회
* `execution_trampoline`: exception을 guest-write event와 serialized worker 요청으로
  연결하는 adapter

두 site index는 `Win32AotCodeCachePlacement`가 소유합니다. initial placement와 dynamic
append가 같은 site 배열을 갱신하며, 배열 개수가 index의 `indexed_site_count`와 다르면
lookup은 답을 거절하고 기존 선형 탐색으로 내려갑니다. 따라서 index는 성능 cache이지
정확성 전제조건이 아닙니다.

현재 안전성은 loader process당 guest 실행 thread 하나를 전제로 합니다. retired
provenance와 generation은 cache 수명 동안 유지되며 reclamation은 아직 없습니다.
REP/string store가 여러 page를 넘는 일반 경우와 multi-thread publication은 후속
검증 범위입니다.

The Win32 AOT file responsibilities also include `aot_inline_cache_site_index`,
which indexes inline-cache miss sites, and `aot_return_dispatch_site_index`,
which indexes the return miss thunk's dispatch sites. The
`aot_return_patch_policy` tracks DBT return-site target diversity and bypasses
repatching after megamorphic classification. Both indexes and the policy belong to
`Win32AotCodeCachePlacement`. A site-count mismatch makes lookup decline and the
caller retain the original linear scan, so these indexes are performance caches
rather than correctness prerequisites.

### AOT self-modifying page generations

AOT placement keeps guest-page provenance, generation, and active state separate
from the immutable address map. A write overlapping translated instruction bytes
causes the serialized worker to replace active entry bytes with `INT3` and remove
them from active guest lookup while preserving cache-to-guest provenance. In the
same writable window, every learned inline-cache guard whose guest target lies on
the retired page is restored to its initial `E9 → miss tail` form (Task 245,
design 238), so learned hits re-enter the dispatcher's normal miss-repatch
protocol instead of trapping forever on the retired entry's `INT3`. Entry
into the retired page snapshots live bytes and publishes the next generation.
Stale entries of at least five bytes are relinked with `E9 rel32`; shorter entries
remain provenance traps. Same-page modification quarantines only that page, while
translation or publication failure follows the generation-failure policy below.
The common path performs no inactive-entry scan.

**Generation-failure scope (Task 415).** A failed re-translation used to quarantine
the entry's whole guest page permanently, dropping every other routine on that page
to single-stepping. One entry is what failed, so the penalty is now the **failing
guest address alone**, held in a 256-entry suppression set and never retried — which
keeps the retry-storm property quarantine provided while the rest of the page keeps
running from the cache. A full suppression set restores the old page-wide quarantine (the
switch that restored it outright was deleted in issue #22). `aot_generation_failure_policy.h`
exposes the counters: failed addresses, skipped attempts, page quarantines, and
spanning activations.

**A requested entry straddling a page boundary (Task 417)** was the root of those
failures. `CanActivateAotAddressMapEntry` refuses an entry that spans a retired
page and exempted **only the requested page**, so once a neighbour retired, a
boundary-straddling requested entry could never activate again and execution fell
back to the arena. The append loop now keeps **the requested entry alone** active
unless it spans a *quarantined* page: the image was just translated from current
guest bytes, and `RegisterAddressMapPages` records the entry under **every** page it
spans, so a later write to either page still retires it. Every other entry keeps the
old rule (the switch that restored the refusal was deleted in issue #22).

### Spin-then-wait for the Glide host-thread rendezvous

Glide commands run on the host thread that owns the GL context, so the guest thread
publishes through `InvokeOnHostThread` and waits for completion. Task 418 measured that
wait at **65.9% of gate time** in pumpit3 (35.2% wake plus 30.5% complete): the fixed
round trip costs about 70,000 cycles against 34,745 cycles of GL work per call, so
**small calls drown in the round trip**. pumpit1, whose calls are far larger and rarer,
spends 7.6% there.

Task 419 replaced the three waits — the pending wait before publishing, the completion
wait, and the host's own pending wait — with a **short spin that falls back to the
condition variable**. The spin pauses with `_mm_pause` while reading `std::atomic<bool>`
mirrors of the two flags, on a budget from `REPIU_GLIDE_RENDEZVOUS_SPIN_US` (default
20 µs, `0` restoring the old behaviour).

**The mirrors are hints only.** A spin that observes its condition still takes the mutex
and re-tests the original condition-variable predicate, which is left unchanged; that is
what prevents lost wakeups. The mirrors are published inside the same critical section as
the flags they mirror, so reading them outside the lock cannot reorder against it.

Measured over a 60-second A/B on pumpit3, `wake + complete` falls from 65.4-66.3% to
**5.3-12.9%** and frames rise from 2,399 to a **median 3,063 (+27.7%)**. Rendezvous
throughput rises 20% while calls per frame stay constant, so the gate did not shrink —
**waiting turned into working**. Spin hit rates are above 99.5% on the guest side and
about 94% on the host side.

The dedicated `aot_page_coherence_win32` subsystem write-watches guest pages that
contain translated instructions as `PAGE_EXECUTE_READ`. On a native guest/cache
store fault, Zydis estimates the write width, only the affected pages become
`PAGE_EXECUTE_READWRITE` for one instruction, and Trap Flag completion restores
RX before retirement is reported. Successful `WriteGuest*` HLE writes report to
the same policy after restoring protection. Only the worker changes the code
cache from RX to RW and back, followed by `FlushInstructionCache`.

The platform-neutral planner also accepts HLE-owned excluded guest ranges.
Synthetic LINEXE/Glide gates become cache sentinels that return to the original
guest address and existing HLE dispatcher instead of copied `UD2` instructions.

`aot_page_coherence_win32` owns page indexing, state, overlap queries,
retirement, and write-watch protection transitions. `aot_code_cache_win32`
orchestrates placement, dynamic append, generation publication, and stale-entry
relinking. The execution trampoline only adapts exceptions into guest-write
events and serialized worker requests. The current safety model assumes one guest
execution thread per loader process. Retired provenance is retained for the cache
lifetime; reclamation, cross-page REP/string stores, and multi-thread publication
remain follow-up work.

## AOT cache breakpoint provenance

guest opcode 기반 boundary reason과 cache `INT3`의 생성 원인은 별도입니다. AOT placement는
정적 image와 동적 append 시 immutable breakpoint offset을 planner HLE, selector guard,
inline-cache fallback, jump-table fallback, other planner fixup으로 O(1) index에 등록합니다.
실행 시점의 retired/inactive entry와 명시적 probe sentinel은 이 index보다 우선합니다.
미등록 주소는 unknown으로 fail-closed합니다. 이 구조는 범용 host dispatch가 HLE나 SMC
provenance를 오인하지 않게 하는 사전 조건이며, hot breakpoint에서는 vector scan을 하지
않습니다.

## AOT cache breakpoint provenance (English)

Guest-opcode boundary reasons and the structural origin of a cache `INT3` are separate.
During static placement and dynamic append, immutable breakpoint offsets are indexed in O(1)
as planner HLE, selector guard, inline-cache fallback, jump-table fallback, or another planner
fixup. Runtime retired/inactive entries and explicit probe sentinels take precedence; an
unindexed address is unknown and fails closed. This prevents a generic host dispatcher from
mistaking HLE or SMC provenance and avoids vector scans on the hot breakpoint path.

## Guarded AOT segment-pop fast path

Task 291은 plain `POP ES/DS/FS/GS`를 planner의 전용 `kGuardedSegmentPop` record로
분류합니다. 플랫폼 공용 emitter는 물리 segment selector, 원래 guest stack word,
shadow selector가 모두 같은 경우에만 EAX/EFLAGS를 보존한 채 guest ESP를 4 증가시키고
cache fallthrough로 이동합니다. 불일치하면 register/flags/ESP를 진입 상태로 복원한 뒤
기존 `INT3`/VEH segment-pop HLE로 fail-closed합니다. `POP SS`와 prefixed form은 기존 HLE
경계입니다.

Win32 정적 배치와 dynamic append는 shadow selector 및 success/fallback counter 주소를
site metadata로 patch합니다. selector table을 아직 사용할 수 없거나 주소를 해결할 수
없으면 slot 시작을 `INT3`로 유지합니다. whole-CFG 검증은 시작 opcode, guard 주소,
fallback `INT3`를 확인하고 cache breakpoint provenance는 fallback을 planner HLE로
계속 집계합니다.

55초 OFF/ON에서 progress는 `37,606 → 39,571`(+5.23%), triangle draw는
`412 → 468`(+13.59%), AOT boundary는 `74,724 → 59,334`(-20.60%)였습니다. ON의
success/fallback은 `21,011/1,593`(92.95% 성공)였고 fatal/AOT legacy fallback은 0,
EEPROM hash는 일치했습니다. 따라서 `dynamic`에서는 항상 켜져 있습니다. 끄기 스위치
`REPIU_AOT_GUARDED_SEGMENT_POP`은 슬롯이 shadow 진실원 방식(v0.0.206, #18)으로 바뀐 뒤
회귀 없이 릴리스가 이어져 issue #30에서 없앴습니다.

```mermaid
flowchart LR
    P["POP ES/DS/FS/GS"] --> G{"physical = stack = shadow?"}
    G -->|yes| C["preserve EAX/EFLAGS<br/>ESP += 4<br/>cache fallthrough"]
    G -->|no| H["restore entry state<br/>INT3 / VEH HLE"]
```

Task 291 classifies plain `POP ES/DS/FS/GS` as dedicated `kGuardedSegmentPop` planner records.
The platform-neutral emitter advances the guest ESP by four and reaches cache fallthrough
only when the physical segment selector, original guest-stack word, and shadow selector are
all equal, preserving EAX and EFLAGS. A mismatch restores registers, flags, and ESP to their
entry state before the existing INT3/VEH segment-pop HLE. `POP SS` and prefixed forms remain
ordinary HLE boundaries.

Static Win32 placement and dynamic append patch shadow-selector and success/fallback counter
addresses through site metadata. An unavailable selector table or unresolved address keeps
the slot entry as INT3. Whole-CFG validation checks the entry opcode, patch locations, and
fallback INT3, while breakpoint provenance continues to classify the fallback as planner HLE.
The 55-second comparison improved progress by 5.23% and triangle draws by 13.59%, reduced AOT
boundaries by 20.60%, and observed a 92.95% guarded success rate with zero fatal/AOT legacy
fallback and matching EEPROM hashes. The path is always on for `dynamic`; its kill switch
`REPIU_AOT_GUARDED_SEGMENT_POP` went in issue #30, after the slots moved onto the shadow in
v0.0.206 (#18) and later releases showed no regression.

## i386 shadow 진실원 세그먼트 슬롯 / Shadow-authoritative i386 segment slots

issue #18(2026-10-06~07)부터 i386(direct 모델)의 guarded **load**·**pop** 슬롯은
물리 segment 레지스터를 읽지 않습니다. `repiu_aot_probe --segment-restore`가
Win32 VEH 재개는 컨텍스트의 segment 쓰기를 무시한다는 것을 확정했으므로(물리
레지스터는 항상 flat selector), 위의 "physical = stack = shadow" 비교는 flat
재로드에서만 통과할 수 있었고 게스트 selector(pumpitea의 0x0024) 로드는 구조적으로
항상 폴백했습니다. 지금은 shadow selector 블록(`AotShadowSelectorBlock`)이 가상
세그먼트 상태의 진실원입니다.

* 블록은 레지스터별 **수용 쌍** `accepted_pair[reg][0..1]`을 가집니다. `[0]`은
  호스트 flat selector(생성 시 씨딩), `[1]`은 HLE 로드가 마지막으로 수용한 base 0
  descriptor의 non-flat selector이며 DPMI set-base(AX=0007)가 그 selector를
  움직이면 0으로 지웁니다. 슬롯은 즉치가 아니라 이 메모리 워드와 비교하므로 쌍이
  바뀌어도 캐시 재패치가 없습니다.
* 슬롯의 새 값(load: 레지스터 또는 재부호화한 메모리 소스, pop: 스택 워드)이
  `[shadow]`와 같으면 no-op 통과, 쌍의 한쪽과 같으면 `[shadow] ← 새 값`을
  네이티브로 쓰고 통과, 그 외에는 진입 상태를 복원해 INT3 폴백(HLE)으로 갑니다.
  pop은 성공 경로에서만 `esp += 4`입니다.
* 메모리-소스 로드(`mov Sreg, r/m16`)는 32-bit 주소, 접두어 0x66/0x2E만, 소스
  세그먼트 CS/DS/SS일 때 `kGuardedSegmentLoad`(`gpr_register =
  kAotSegmentLoadMemorySource`)로 분류되고, 이미터가 ModRM을 ax 목적지로
  재부호화합니다(ESP 베이스는 슬롯의 push 2개만큼 +8). pumpitea INT8 ISR의
  `66 2E 8E 1D disp32`가 동기입니다.
* 엔진은 shadow→`guest_*` 방향으로 동기합니다(`BuildAotSegmentTable`,
  `SyncGuestSegmentsFromShadow`, `SetGuestSegmentSelector`). 전량 재패치는 네이티브
  flip마다가 아니라 다음 HLE 재해석이 바뀐 shadow를 볼 때 한 번 일어납니다.
* i386 **read** 슬롯(`mov r16/r32, Sreg`)은 가드 없이 `[shadow]`를 16-bit로
  로드하고 fallthrough로 뜁니다(12바이트). HLE도 shadow를 돌려주므로 물리 비교는
  결과를 바꾸지 않고 트랩만 더했습니다.
* INT8 주입(`InjectPendingInterrupts`)은 direct 모델에서 벡터 주소를
  `FindAotCacheAddress`로 찾아 캐시 블록으로 진입하고, 없으면
  `RequestAotDynamicTranslation`을 한 번 호출합니다. 이전에는 원 게스트 주소로
  들어가 ISR 앞부분이 캐시 밖에서 네이티브로 돌며 폴트(세그먼트 로드, 포트 I/O)로
  HLE에 들어왔습니다. `REPIU_TIMER_HANDLER_CACHE_ENTRY=0|off|false`로 끕니다.
* long-mode(x64 cache 모델) 슬롯은 그대로이며, 메모리-소스 로드는 x64에서 INT3
  boundary로 남습니다.

```mermaid
flowchart TD
    N["새 값: 레지스터 / 메모리 소스 / 스택 워드"] --> A{"== [shadow]?"}
    A -->|예| S["통과 (pop: esp += 4)"]
    A -->|아니오| B{"== [pair0] 또는 [pair1]?"}
    B -->|예| W["[shadow] ← 새 값"] --> S
    B -->|아니오| F["INT3 → HLE load/pop"]
```

Since issue #18 (2026-10-06/07) the i386 (direct model) guarded **load** and
**pop** slots read no physical segment register. `repiu_aot_probe
--segment-restore` established that a Win32 VEH resume ignores segment writes
in the context (the physical registers always hold the flat selector), so the
"physical = stack = shadow" compare above could pass only for flat reloads and
guest-selector loads (pumpitea's 0x0024) structurally always fell back. The
shadow selector block (`AotShadowSelectorBlock`) is now the source of truth for
the virtual segment state. It carries a per-register **accepted pair**
`accepted_pair[reg][0..1]`: the host flat selector (seeded at creation) and
the last base-0, descriptor-backed non-flat selector the HLE load accepted,
cleared when DPMI set-base moves it; the slots compare against these memory
words, not immediates, so pair changes never re-patch the cache. A slot's new
value (load: register or re-encoded memory source; pop: stack word) equal to
`[shadow]` passes as a no-op, equal to either pair word writes the shadow
natively and passes, and anything else restores entry state and falls back to
the HLE through INT3; the pop consumes the stack dword on success only.
Memory-source loads (`mov Sreg, r/m16`) classify as `kGuardedSegmentLoad` with
`gpr_register = kAotSegmentLoadMemorySource` under 32-bit addressing, prefixes
0x66/0x2E only and a CS/DS/SS source, and the emitter re-encodes the ModRM to an
`ax` destination (+8 on an ESP base for the slot's two pushes); pumpitea's INT8
ISR entry `66 2E 8E 1D disp32` motivated it. The engine syncs shadow →
`guest_*` (`BuildAotSegmentTable`, `SyncGuestSegmentsFromShadow`,
`SetGuestSegmentSelector`), and the whole-cache re-patch fires once at the next
HLE re-resolution that sees a changed shadow rather than per flip. The i386
**read** slot (`mov r16/r32, Sreg`) loads `[shadow]` unguarded as a 16-bit
value and jumps on (12 bytes): the HLE returns the shadow too, so the former
physical compare only added traps. INT8 injection (`InjectPendingInterrupts`)
on the direct model enters the handler's cache block found by
`FindAotCacheAddress`, requesting one `RequestAotDynamicTranslation` when
unmapped; it used to enter the raw guest vector, so the handler's first
instructions ran natively outside the cache and reached the HLE through faults
(segment loads, port I/O). `REPIU_TIMER_HANDLER_CACHE_ENTRY=0|off|false`
disables it. Long-mode slots are unchanged; memory-source loads stay INT3
boundaries on x64.

## 네이티브 span 음성 캐시 / Native-span negative cache

Task 304는 기본 native linear-span scan이 0~1개 일반 명령 뒤 정적 경계에서 거절한
결과를 entry EIP별로 저장합니다. 항목은 최대 30바이트의 guest snapshot을 가지며, 현재
바이트가 완전히 같을 때만 Zydis 재디코딩을 생략하고 기존 single-step fallback을
선택합니다. 바이트가 다르면 stale 항목을 삭제하고 재스캔합니다. 캐시는 어떤 명령도
추가로 native 실행하지 않으므로 SMC/AOT generation 정책보다 보수적인 fallback
최적화입니다.

항목 수는 65,536개로 제한합니다(이 캐시를 우회하던 write/jump 실험 mode는 issue #22에서
삭제). 세 번의 60초 A/B에서 거절 hit율은 99.68~99.69%였고
texture milestone 중앙값은 1,031ms(약 4.9%) 빨라졌습니다. fatal/legacy fallback은 0,
EEPROM hash는 일치했습니다. 따라서 `dynamic`에서 켜지고 다른 backend에서는 꺼집니다.
설정 변수 `REPIU_NATIVE_LINEAR_SPAN_REJECT_CACHE`는 #20에서 제거되었습니다.

Task 304 caches default native linear-span scans that reject at a static boundary after zero
or one ordinary instruction. Each entry holds up to 30 guest bytes. Only an exact byte match
skips Zydis decoding and selects the existing single-step fallback; a mismatch erases the
stale entry and rescans. A hit never permits additional native execution, making this a
conservative fallback optimization independent of SMC/AOT generation state.

The cache is capped at 65,536 entries (the write and jump experiments that bypassed it were
deleted in issue #22). Three 60-second A/B pairs observed a 99.68-99.69% rejection hit rate and a
1,031ms median texture-milestone improvement (about 4.9%), with zero fatal/legacy fallback
and matching EEPROM hashes. It is default-on for `dynamic`, default-off elsewhere, and
disabled by `0|off|false` or unknown settings.

## Retired trap 즉시 native span 후보 (삭제됨) / Immediate native span after retired traps (deleted)

Task 305는 retired cache `INT3`의 해결이 실패한 직후 native linear-span scanner를 부르는
옵트인(`REPIU_AOT_RETIRED_SPAN_REENTRY`)을 더했습니다. 세 번의 30초 교차 A/B에서
single-step은 중앙값 2.86% 줄었지만 progress 개선이 0.35%에 그쳐 기본 OFF로 남았고,
issue #22에서 스위치, 시도·성공 카운터(`retired_span=`), 실행 시간 버킷과 함께
삭제했습니다.

Task 305 added an opt-in (`REPIU_AOT_RETIRED_SPAN_REENTRY`) that called the native
linear-span scanner right after a retired cache `INT3` failed to resolve. Across three
30-second alternating pairs it cut single-steps by a 2.86% median but improved progress by
only 0.35%, so it stayed default off; issue #22 deleted it with its switch, its attempt and
success counters (`retired_span=`) and its execution-time bucket.

## Retired trap hotset 계측 / Retired-trap hotset profiling

Task 306은 `REPIU_AOT_RETIRED_TRAP_PROFILE=1|on|true`에서 retired cache `INT3`의
guest/cache 주소, inactive entry 세대, guest/emitted 길이와 resolver 결과를 집계합니다.
guest/cache histogram은 각각 65,536개 주소로 제한되며 초과 횟수를 별도로 보존합니다.
종료 snapshot은 guest/cache 상위 16개와 guest top-16 coverage, 5바이트 relink 가능 여부,
active/generation/quarantine/failure/fallback/trace 결과별 횟수를 출력합니다. 기본값은
OFF이며 비활성 상태에서는 histogram lookup을 수행하지 않습니다.

60초 profile은 retired trap `7,401`회 중 `7,293`회(98.54%)가 emitted length 1~4인
짧은 entry이고 동일하게 quarantine 결과였음을 확인했습니다. 상위 guest 16개는 전체의
98.24%, `0x030F4A94`와 `0x030F507C` 두 주소는 64.06%를 차지했습니다. relink 가능한
108회는 generation publish 107회와 failure 1회로 분리됐습니다. 따라서 기존 5바이트
`E9 rel32` 재연결을 확대하는 것보다 짧은 retired entry를 예외 없이 우회하는 공용
side-table/dispatch 경계가 다음 후보입니다.

Task 306 profiles retired cache `INT3` events only under
`REPIU_AOT_RETIRED_TRAP_PROFILE=1|on|true`. It records guest/cache addresses, inactive-entry
generation and lengths, and resolver outcomes in separately capped 65,536-entry histograms.
The final snapshot reports guest/cache top 16 lists, guest top-16 coverage, five-byte relink
eligibility, and active/generation/quarantine/failure/fallback/trace counts. Profiling defaults
off and performs no histogram lookup while disabled.

The 60-second profile found that 7,293 of 7,401 retired traps (98.54%) came from one-to-four
byte entries and resolved as quarantine. The top 16 guest addresses covered 98.24%; just
`0x030F4A94` and `0x030F507C` covered 64.06%. The 108 relinkable traps split into 107
generation publications and one failure. A shared side-table or dispatch boundary that
redirects short retired entries without an exception is therefore the next candidate, rather
than extending the existing five-byte `E9 rel32` relink.

## Exception-free superblock 검증 경계 / Exception-free superblock validation boundary

Task 308은 기존 AOT planner/emitter가 이미 cache-local direct call/jump, conditional,
fallthrough와 backward edge를 연결한다는 사실을 기준으로 합니다. 새 superblock은
기본 블록 형식을 다시 만드는 기능이 아니라, 일반 planner HLE `INT3`를 정상 host-call
dispatch slot으로 대체하는 opt-in 실행 경계입니다.

`REPIU_AOT_DBT_SUPERBLOCK=1|on|true`에서 Win32 x86 thunk는 GPR/EFLAGS와
x87/MMX/SSE를 저장하고 host stack 및 TIB stack bounds로 전환한 뒤 기존 공용 HLE
handler chain을 호출합니다. 처리된 다음 guest EIP에 활성 cache entry가 있으면 직접
복귀합니다. segment register write, invalid site, 미처리 HLE, target miss 또는 상태
계약 실패는 기존 planner-HLE provenance `INT3`로 fail-closed합니다. SMC write-watch와
generation publication은 기존 정책을 그대로 사용합니다.

Task 308 is based on the confirmed fact that the existing AOT planner and emitter already
chain cache-local direct calls/jumps, conditional branches, fallthroughs, and backward edges.
The new superblock boundary is therefore an opt-in replacement of ordinary planner-HLE
`INT3` exits with a normal host-call dispatch slot, not a second basic-block format.

Under `REPIU_AOT_DBT_SUPERBLOCK=1|on|true`, the Win32 x86 thunk preserves GPR/EFLAGS
and x87/MMX/SSE state, switches to the host stack and TIB stack bounds, and invokes the
shared HLE handler chain. A handled next guest EIP resumes at an active cache entry.
Segment-register writes, invalid sites, unhandled HLE, target misses, and state-contract
failures fail closed through the existing planner-HLE provenance `INT3`. Existing SMC
write-watch and generation-publication policy remains unchanged.

첫 unrestricted 실행은 정상 호출 ABI의 안전 범위를 추가로 확인했습니다.
`0x030F5D27: INT 21h AH=25h`를 직접 처리하면 INT 8 vector selector가 기존 VEH
경로의 `002B` 대신 `0023`으로 저장되어 후속 `0x03042EBE` AV를 만들었습니다.
따라서 first slice는 segment/ESP write 외에 모든 `INT/IRET`도 VEH 경계에 남깁니다.
target miss는 HLE가 이미 committed된 경우 source를 재실행하지 않고 처리된 다음
guest EIP에서 TF bridge로 들어갑니다.

60초 OFF/ON 검증 결과는 다음과 같습니다.

| 지표 | OFF | ON | 변화 |
|---|---:|---:|---:|
| progress | 44,977 | 45,716 | +1.64% |
| single-step | 276,680 | 254,889 | -7.88% |
| AOT boundary | 66,245 | 41,224 | -37.77% |
| host-call HLE success/fallback | 0/0 | 25,134/19,196 | - |

양쪽 모두 exception/legacy fallback 0, Glide gate 4,582/4,582, 동일한 Glide gap
count와 EEPROM hash를 유지했습니다. 정상 호출 HLE 경계는 안전 subset에서 유효하지만
5배 whole-run go/no-go에는 실패했습니다. 일반 HLE 예외 제거를 60배 목표의 주
아키텍처로 확장하지 않습니다.

The unrestricted run showed that direct `INT 21h AH=25h` changes the established INT 8
selector contract and later faults. All `INT/IRET` forms therefore remain VEH-mediated
along with segment/ESP writes; a post-commit target miss resumes the handled next EIP through
the TF bridge. The controlled 60-second pair reduced single-step by 7.88% and AOT boundaries
by 37.77%, but improved progress only 1.64%. Both runs preserved zero exception/legacy
fallback, identical Glide activity and gap counts, and matching EEPROM hashes. The safe
normal-call boundary is viable, but fails the 5x architecture gate.

## Single-step hotspot latency 계측 / Single-step hotspot latency profiling

Task 309는 기본 OFF인 `REPIU_SINGLE_STEP_HOTSPOT_PROFILE=1|on|true` 계측을
추가합니다. 계측을 켠 실행만 8,192-slot open-addressing table을 heap에 할당하며,
guest thread는 allocation과 lock 없이 `HandleSingleStepTrace` 진입 EIP별 count,
total/max TSC tick과 HLE/timer/native/TF outcome을 누적합니다.

```mermaid
flowchart LR
    D["#DB → HandleSingleStepTrace"] --> R["RAII TSC scope"]
    R --> O["HLE / timer / native / TF outcome"]
    O --> H["guest-EIP fixed histogram"]
    H --> C["count top 32 + coverage"]
    H --> T["TSC tick top 32 + coverage"]
```

종료 시에만 histogram을 snapshot하고 count와 TSC tick 순위를 독립 정렬합니다.
계측 범위는 handler body이며 kernel의 #DB 진입 전과 VEH 복귀 후를 포함하지 않습니다.
또한 thread preemption이 sample latency를 늘릴 수 있으므로 값을 순수 CPU cycle 또는
전체 예외 전환 비용으로 해석하지 않습니다.

60초 검증에서는 HLE가 event의 33.60%와 handler tick의 84.82%를 차지했습니다.
상위 주소는 segment-register move와 port-I/O HLE에 분산됐고 cycle 상위 32 coverage는
67.21%였습니다. 단일 loop의 exception-free generation은 80% gate를 통과하지
못했으므로, 다음 single-step 작업은 상위 HLE의 opcode-directed dispatch/decode
비용과 계측 범위 밖 exception transition 비용을 먼저 분리합니다.

Task 309 adds an opt-in `REPIU_SINGLE_STEP_HOTSPOT_PROFILE=1|on|true` profile. Enabled runs
allocate an 8,192-slot open-addressing table on the heap. The guest thread records per-entry
EIP count, total/max TSC ticks, and HLE/timer/native/TF outcomes without allocation or locks.
The final snapshot independently sorts the top 32 by count and by ticks.

The scope covers the handler body, not kernel #DB entry or the path after VEH returns, and
preemption can inflate latency. In the 60-second profile, HLE accounted for 33.60% of events
and 84.82% of handler ticks. Segment-register and port-I/O HLE sites were distributed across
the ranking, and the top 32 covered 67.21%, below the 80% gate for generating one
exception-free loop. The next single-step task must first separate hot-HLE dispatch/decode
work from the exception-transition cost outside this scope.

## 동적 DX 포트 출력 래퍼 보존 / Dynamic DX-port output wrapper preservation

Watcom 런타임은 하나의 `OUT DX,AL/AX/EAX` 명령을 포함한 공용 함수를 여러 장치 출력에
재사용합니다. 따라서 포트 I/O HLE가 무시하거나 관찰만 하는 출력도 게스트 명령을 NOP로
영구 패치하지 않습니다. 에뮬레이터는 현재 EIP만 명령 길이만큼 전진해 그 트랜잭션을
버리고, 래퍼의 바이트는 보존하여 다음 호출의 DX 값이 다른 장치로 전달되게 합니다.
이 정책은 target, 실행 주소, 포트 번호와 무관하며 에뮬레이션 대상 출력에도 동일하게
EIP 전진 방식을 사용합니다.

The Watcom runtime reuses shared functions containing one `OUT DX,AL/AX/EAX` instruction for
multiple devices. Port I/O HLE therefore never permanently NOP-patches guest code for an output
that is ignored or only observed. The emulator advances only the current EIP by the instruction
length, discarding that transaction while preserving the wrapper bytes so a later DX value can
reach a different device. This policy is independent of target, executable address, and port
number; emulated output paths use the same EIP-advance behavior.

## 포트 I/O 지연 루프 batching / Port I/O delay-loop batching

Task 414는 **결과를 쓰지 않는 포트 폴링 루프**를 인식해 반복을 건너뜁니다. pumpit3의
타이머 ISR은 tick(240 Hz)마다 `inc r; sub eax,eax; in ax,dx; cmp r,imm; jl` 루프를 200회
돌고, 루프 종료 직후 `mov eax,[...]`가 EAX를 덮어씁니다. 즉 읽은 값은 **한 번도 쓰이지
않는** 순수 지연이며, 우리에겐 반복마다 CPU fault 1회입니다.

```mermaid
flowchart LR
    F["in ax,dx → fault"] --> E["기존 포트 emulate"]
    E --> M{"루프 모양 일치?"}
    M -->|아니오| R["예전 경로 그대로"]
    M -->|예| C["카운터 := limit - step"]
    C --> G["게스트가 마지막 반복 1회를 직접 실행"]
    G --> X["cmp/jcc가 자연스럽게 종료"]
```

* **레지스터 하나만 씁니다.** EIP·EFLAGS·EAX·EDX를 만들지 않으므로 종료 상태가 원래
  실행과 같고, 플래그 합성의 등가성을 증명할 필요가 없습니다.
* 일치 조건: `IN` 뒤가 `cmp r32, imm` + **뒤로 가는 signed 조건 분기**, 본문은
  `inc`/`dec`/자기 0화(`sub r,r`·`xor r,r`)뿐, 본문이 `IN` 앞에서 EAX를 0으로 만들 것
  (**건너뛰는 읽기가 죽은 값이라는 증명**), 카운터가 EAX·EDX가 아닐 것, 남은 반복 2회
  이상, 디코드 범위가 읽기 가능할 것. 하나라도 어긋나면 **아무 상태도 바꾸지 않습니다.**
* **부수 효과 없는 입력 경로에서만** 시도합니다(JAMMA 입력). EEPROM·YMZ280B 창은 앞선
  분기에서 처리되므로 대상이 아닙니다.
* 항상 켜져 있으며(끄기 스위치 `REPIU_PORT_IO_DELAY_LOOP`는 #20에서 제거), 통계(시도·
  batch·건너뛴 반복·불일치 사유)를 로그로 냅니다.

Task 414 recognises a **port polling loop whose result is never used** and skips its
iterations. pumpit3's timer ISR runs `inc r; sub eax,eax; in ax,dx; cmp r,imm; jl` 200 times
per 240 Hz tick and overwrites EAX immediately after the loop, so the reads are a pure delay
that cost us one CPU fault each. On a match the handler writes **one register** — the
counter — so the guest executes its own final iteration and the exit state is identical
without synthesising flags. The match requires `cmp r32, imm` plus a backward signed branch
after the `IN`, a body of only `inc`, `dec`, or self-zeroing `sub`/`xor`, that body zeroing
EAX before the `IN` (**the proof that skipped reads are dead**), a counter that is neither
EAX nor EDX, at least two iterations remaining, and readable bytes; any mismatch changes
nothing. It is attempted **only on the side-effect-free JAMMA input path**, since the EEPROM
and YMZ280B windows are handled by earlier branches. It is always on (the
`REPIU_PORT_IO_DELAY_LOOP` kill switch was removed in #20), and the attempt, batch,
skipped-iteration, and refusal-reason counts are logged.

Task 737은 같은 루프의 **조건을 뒤집은 형식**을 받아들입니다. pumpitea의 입력 스캔은
`cmp ebx,200; jge exit; jmp back`이라 Task 414의 `jl back` 모양과 어긋났고, Win32에서는 IN 한 번의
예외 왕복(약 20 µs) × 200 × 240 Hz가 CPU 전체를 써서 게임이 35초 뒤 멈췄습니다. `IN` 뒤 `cmp` 다음이
**앞으로 가는** `jge`/`jg`(짧은 형식과 `0F 8D`/`0F 8F`)이고 그 뒤가 짧은 무조건 `jmp`이면 조건을
`jl`/`jle`로 뒤집고 `jmp`의 변위로 본문 시작을 구해 기존 검증에 넘깁니다. 본문 모양, EAX 0화 증명,
카운터 레지스터, 남은 반복 수 규칙은 그대로이며, 배치 뒤 guest는 여전히 마지막 반복을 직접 실행합니다.
pumpitea에서 Win32 90초에 19,001회 배치로 읽기 376만 회를 건너뛰었고 pumpit2a에서는 일치가 없습니다.

Task 737 accepts the **inverted form** of the same loop. pumpitea's input scan is `cmp ebx,200; jge
exit; jmp back`, which Task 414's `jl back` shape refused, and on Win32 one IN's exception round trip
(about 20 µs) × 200 × 240 Hz consumed the whole CPU, stalling the game after 35 s. When the `cmp` after
the `IN` is followed by a **forward** `jge`/`jg` (short form or `0F 8D`/`0F 8F`) and then a short
unconditional `jmp`, the condition is inverted to `jl`/`jle`, the body start is taken from the `jmp`'s
displacement, and the existing checks take over. The body shape, the EAX-zeroing proof, the counter
register and the remaining-iteration rules are unchanged, and after a batch the guest still runs the
final iteration itself. On pumpitea, 90 s of Win32 made 19,001 batches skipping 3.76 M reads; pumpit2a
has no match.

Task 470은 같은 정책을 **호출 래퍼형 입력 루프**로 확장합니다. 제한된 래퍼가 EDX를
stack에 저장하고 EAX의 포트 번호를 EDX로 옮긴 뒤 EAX를 0으로 만들고 `IN`, EDX 복원,
`RET`을 수행하는지 검증합니다. guest 반환 주소 앞의 `call rel32`가 그 래퍼를 가리키고,
호출자가 `inc edx; cmp edx,imm; jl`로 반복하며 루프 머리의 `mov eax,imm`가 이전 입력
결과를 폐기하는 경우에만 stack의 저장된 EDX를 `limit-2`로 전진시킵니다. 원본 `POP`,
마지막 입력, `INC/CMP/JL`은 guest가 실행하므로 EIP와 flags를 합성하지 않습니다. 이
matcher도 target 이름이나 고정 주소를 사용하지 않으며 모든 불확실성에서 기존 경로로
닫힙니다.

Task 470 extends the same policy to **call-wrapped input loops**. It proves a restricted wrapper
that saves EDX on the stack, moves the EAX port argument to EDX, clears EAX, executes `IN`, restores
EDX, and returns. Only when the `call rel32` before the guest return address targets that wrapper,
the caller repeats through `inc edx; cmp edx,imm; jl`, and `mov eax,imm` at the loop head discards
the prior input result does it advance the saved stack value to `limit-2`. The guest still executes
the original `POP`, final input, and `INC/CMP/JL`, so no EIP or flags are synthesized. This matcher
also uses neither target names nor fixed addresses and fails closed to the existing path.

## 타임스탬프 기반 JAMMA 입력 재생 / Timestamped JAMMA input replay

Task 492부터 Win32 JAMMA 입력은 live level과 timer replay를 구분합니다. SDL event pump는
매핑된 key down/up의 원래 nanosecond timestamp와 edge 뒤 전체 pressed mask를 고정 용량
timeline에 기록합니다. PIT scheduler는 due count뿐 아니라 epoch, 첫 tick ordinal,
divisor를 제공하며, delivery accounting이 실제 수락한 최대 64개 tick의 absolute due
timestamp만 별도 큐에 넣습니다.

```mermaid
flowchart LR
    E["SDL key edge + timestamp"] --> H["pressed-mask history"]
    P["PIT due ordinal"] --> Q["accepted due-time queue"]
    Q --> I["INT 8 injection"]
    H --> R["state at tick due time"]
    I --> R
    R --> G["original ISR JAMMA IN"]
```

INT 8 주입은 가장 오래된 due timestamp와 interrupt-frame ESP를 활성화합니다. IF=0이고
현재 ESP가 그 frame 이하인 동안의 JAMMA 읽기만 과거 state를 사용하며 IRETD가 IF 또는
ESP를 복원하면 live 경로로 돌아갑니다. replay는 500 us wall-clock snapshot을 우회하므로
빠르게 소진되는 overdue tick들이 같은 현재 state로 합쳐지지 않습니다. ISR 밖에서는
기존 `GetAsyncKeyState`와 Task 403 cache가 그대로 유지됩니다. 입력/tick 큐는 allocation
없는 고정 배열과 짧은 spin guard를 사용하고, timer accounting과 due queue의 수락·소비는
같은 delivery guard 아래에서 처리합니다. timeout 강제 종료 뒤 진단 snapshot은 guard를
취하지 않아 종료된 guest가 소유하던 lock을 기다리지 않습니다.

Since Task 492, Win32 JAMMA input distinguishes live level sampling from timer replay. The SDL
event pump records each mapped key down/up with its original nanosecond timestamp and the complete
post-edge pressed mask in a fixed timeline. The PIT scheduler exposes the epoch, first due ordinal,
and divisor as well as the count, while only timestamps for ticks actually retained by delivery
accounting enter the 64-entry due queue.

INT 8 injection activates the oldest due timestamp and interrupt-frame ESP. Only JAMMA reads with
IF clear and ESP at or below that frame reconstruct historical state; restoration of IF or ESP by
IRETD returns to the live path. Replay bypasses the 500 us wall-clock snapshot so rapidly drained
overdue ticks do not collapse onto one current state. Outside the ISR, the existing
`GetAsyncKeyState` and Task 403 cache remain unchanged. Fixed arrays and short spin guards avoid
allocation, and delivery accounting plus due-queue acceptance/consumption share one guard. A
post-timeout diagnostic snapshot takes no guard, so it cannot wait on one formerly held by a
terminated guest.

### Task 493 IRQ0 프레임 수명 보정 / Task 493 IRQ0 frame-lifetime correction

Task 492의 최초 조건은 IF가 꺼진 경우만 ISR 내부로 보았지만, 실제 handler는 JAMMA 읽기
전에 IF를 다시 켭니다. Task 493부터 replay 범위는 IF가 아니라 host가 주입한 interrupt
frame의 stack 수명으로 결정합니다. 고정 64-entry frame stack은 `(due timestamp, frame
ESP)`를 저장하고, 새 주입 전과 JAMMA read 시 `current ESP > frame ESP`인 완료 frame을
제거합니다. nested IRQ0에서는 가장 안쪽 due state를 사용하고, 그 frame이 IRETD로
복귀하면 바깥 due state가 다시 보입니다. 활성 frame이 없을 때만 기존 live level/cache
경로를 사용합니다.

Task 492 initially treated only IF-clear execution as ISR scope, but the observed handler enables
IF before its JAMMA reads. Since Task 493, replay scope follows the stack lifetime of host-injected
interrupt frames instead. A fixed 64-entry stack stores `(due timestamp, frame ESP)` and retires
completed frames for which `current ESP > frame ESP` before each injection and JAMMA read. Nested
IRQ0 uses the innermost due state and reveals the outer due state again after nested IRETD. Only an
empty frame stack falls back to the existing live-level/cache path.

### Task 494 2P 숫자패드 별칭 / Task 494 2P numpad aliases

2P 입력의 각 물리 위치는 SDL navigation key와 numeric keypad key를 같은
`JammaInputKey`로 정규화합니다. `Home/KP7`, `PageUp/KP9`, `Clear/KP5`, `End/KP1`,
`PageDown/KP3` 쌍은 SDL edge history, Win32 초기 snapshot, replay 밖 live scan에 모두
동일하게 적용됩니다. 따라서 NumLock 상태는 JAMMA port bit와 due-time replay 결과를
바꾸지 않습니다.

Each physical 2P position normalizes its SDL navigation and numeric-keypad aliases into one
`JammaInputKey`. The pairs `Home/KP7`, `PageUp/KP9`, `Clear/KP5`, `End/KP1`, and `PageDown/KP3`
apply equally to SDL edge history, initial Win32 snapshot capture, and the live scan outside replay.
NumLock state therefore does not change the JAMMA port bit or due-time replay result.

### Task 495 JAMMA history 안전 정리 / Task 495 safe JAMMA history pruning

입력 history는 pending timer due와 active IRQ0 replay frame 중 가장 오래된 timestamp를
안전 cutoff로 사용합니다. cutoff 이하의 마지막 state 하나를 floor에 남기고 나머지 edge를
정리하므로, 완료된 입력이 고정 capacity를 영구히 점유하지 않습니다. 둘 다 비어 있으면
단조 증가하는 마지막 PIT due timestamp를 cutoff로 사용합니다. 안전 정리 뒤에도 capacity가
가득 찬 경우만 `history-overflow`이고, floor보다 오래된 state를 요청한 경우는 별도의
`history-coverage-miss`입니다. `history-pruned`와 `history-peak`가 정상 정리량과 실제 보존
범위를 보여 줍니다.

Input history uses the oldest timestamp among pending timer due entries and active IRQ0 replay
frames as its safe cutoff. It keeps the last state at or before that cutoff as the floor and retires
earlier edges, preventing completed input from permanently occupying fixed capacity. When neither
source exists, the monotonically increasing last PIT due timestamp becomes the cutoff. Only a full
buffer remaining after safe pruning counts as `history-overflow`; a request older than the floor is
reported separately as `history-coverage-miss`. `history-pruned` and `history-peak` expose normal
retirement volume and the actual retained range.


### issue #58 PIU10 frame-sync의 tick 시각 응답 / PIU10 frame-sync answered at tick time

PIU10 게임은 타이머 핸들러에서 MP3 frame-sync(0x2DA status bit 2)의 변화를 세어 곡 위치를 정하고, 256프레임마다
화살표가 읽는 곡 시계를 그 위치로 덮어씁니다. tick이 늦게 몰려 들어가면 핸들러들이 같은 순간의 비트를 읽어 변화
두 개가 상쇄되므로, frame-sync도 JAMMA 입력과 같은 timeline을 씁니다. `Piu10Mp3AudioOut` 워커가 뒤집은 시각을
`Piu10FrameSyncHistory`에 기록하고, 상태 읽기는 `JammaInputTimeline::TryReplayTimestamp`가 주는 게스트 시각
(핸들러 안은 그 tick의 due, 밖은 마지막으로 전달된 tick의 due)에서 `Piu10FrameSyncAnswerClock`이 뒤로 가지 않게
고정한 시각의 비트로 답합니다. 핸들러 밖 읽기(같은 포트의 CAT702 통신)를 지금 시각으로 답하면 답이 핸들러의
tick보다 앞서므로 쓰지 않습니다. 답은 이력의 값에서만 나오며(live 비트를 쓰면 뒤집기와 기록 사이의 race로 가짜
변화가 생김), `REPIU_PIU10_FRAME_SYNC_REPLAY=0`이 이전 동작입니다.

The PIU10 games count MP3 frame-sync changes (0x2DA status bit 2) in their timer handler to know the song position
and overwrite the song clock the arrows read with it every 256 frames. With ticks arriving late and bunched, handlers
read the bit at one instant and two changes cancel, so frame-sync uses the JAMMA inputs' timeline as well. The
`Piu10Mp3AudioOut` worker records each flip in `Piu10FrameSyncHistory`, and a status read is answered with the bit at
the guest's time from `JammaInputTimeline::TryReplayTimestamp` (the tick's due time inside a handler, the latest
delivered tick's outside), kept from running backwards by `Piu10FrameSyncAnswerClock`. Reads outside handlers (the
CAT702 exchange on the same port) are not answered at the present, which would run ahead of the handlers' ticks.
Answers come from the history alone (the live bit races with the recording of a flip and made phantom changes);
`REPIU_PIU10_FRAME_SYNC_REPLAY=0` restores the previous behavior.

## 게스트 위치 census / Guest position census

Task 411은 기본 OFF인 `REPIU_GUEST_POSITION_CENSUS=1|on|true` 계측을 추가합니다.
위 single-step 핫스팟 계측과 목적이 다릅니다 — **표본 시점이 예외에 묶여 있지
않습니다.** poll thread가 `REPIU_GUEST_POSITION_CENSUS_MS`(기본 10, 1~1000으로 clamp)
간격마다 게스트 스레드를 정지시켜 EIP를 읽고, 4,096-slot open-addressing table에
누적합니다.

```mermaid
flowchart LR
    P["poll thread<br/>간격 만료"] --> S["SuspendThread<br/>GetThreadContext"]
    S --> M{"EIP 위치"}
    M -->|"cache 범위"| G["FindAotGuestAddress<br/>→ guest 주소"]
    M -->|"arena 범위"| A["guest 주소 그대로"]
    M -->|"그 외"| H["host 주소"]
    G --> T["주소별 histogram + origin"]
    A --> T
    H --> T
    T --> D["종료 시 상위 16 로그 + 전체 덤프"]
```

* 캡처는 기존 `CaptureWin32NativePhaseSample`을 재사용하며, suspend와 resume 사이에서
  할당·락·I/O를 하지 않습니다. 역매핑은 EIP가 캐시 범위일 때만 수행합니다 — 번역
  worker는 게스트 스레드가 자신을 기다릴 때만 placement를 바꾸므로 그때 address map은
  안정입니다.
* `origin`(arena / cache-mapped / cache-unmapped / host)별 합계를 따로 세어
  **`합 == total` 검산**을 로그에 함께 출력합니다. 검산이 깨지면 분포로 읽지 않습니다.
* 캐시 표본은 게스트 주소로 접혀 arena 표본과 같은 축에서 합산되므로, 같은 게스트
  코드가 두 실행 방식으로 나뉘어도 한 줄로 보입니다.
* 기존 `REPIU_NATIVE_SAMPLING` 표본기는 **예외 dispatch가 1초간 조용해야** 발화하므로
  멈춘 실행에서는 한 번도 동작하지 않습니다. 이 census는 그 게이트를 쓰지 않습니다.
* census를 켠 실행은 suspend/resume 비용이 붙으므로 **wall·프레임을 인용하지
  않습니다.**

Task 412는 여기에 **host 시간 귀속**을 더합니다.

* **스레드 CPU 시간** — 표본마다 `GetThreadTimes`를 읽어 kernel/user 시간과 wall을
  함께 보고합니다. host 표본이 많을 때 **"커널에서 바쁨"과 "어딘가에서 막힘"을 가르는
  단일 판정**입니다.
* **host 호출 지점** — 표본이 host이면 정지 상태에서 `ESP`부터 최대 64 dword를 훑어
  **로더 모듈 범위 안의 첫 값**을 별도 표(1,024 slot)에 누적합니다. 프레임 체인을
  신뢰하지 않는 후보 탐색이므로 **정식 스택 워크가 아니며**, 상위 몇 개를 분포로
  읽습니다. 읽기는 C++ 객체 없는 함수에서 SEH로 감싸고, 결과는
  `sited`/`no-site`/`failed` 셋 중 정확히 하나로 계상되어 **합이 host 표본 수와 같아야
  합니다.**
* **모듈·심볼** — 종료 후 `GetModuleHandleExA`로 모듈을, `dbghelp`로 함수명을
  해석합니다. Release는 `/Zi` + `/DEBUG`로 PDB를 만들되 **최적화 플래그는 그대로**라
  코드 생성은 바뀌지 않습니다. 심볼이 없으면 `모듈+offset`으로 물러섭니다.

Task 412 adds **host-time attribution** on top: `GetThreadTimes` per sample, reported
against wall clock, is the single split between "busy in the kernel" and "blocked
somewhere"; host samples also get a shallow scan of up to 64 dwords from `ESP` for the
first value inside the loader's image, accumulated in a separate 1,024-entry table — a
candidate search rather than a stack walk, read as a distribution, wrapped in SEH inside a
function with no C++ objects, and counted as exactly one of `sited`, `no-site`, or `failed`
so the three must sum to the host sample count. Modules are resolved with
`GetModuleHandleExA` and symbols with `dbghelp` after the run; Release now carries `/Zi`
and `/DEBUG` for the PDB while **keeping every optimisation flag**, so code generation is
unchanged and a missing symbol degrades to `module+offset`.

Task 411 adds an opt-in `REPIU_GUEST_POSITION_CENSUS` census whose sampling instant is
**not tied to an exception**, unlike the single-step hotspot profile above. The poll thread
suspends the guest thread every `REPIU_GUEST_POSITION_CENSUS_MS` milliseconds (default 10,
clamped to 1-1000), reads EIP, and accumulates it in a 4,096-slot open-addressing table.
Capture reuses `CaptureWin32NativePhaseSample` and allocates, locks, and performs no I/O
between suspend and resume; the reverse map runs only for cache-range addresses, where the
translation worker cannot be mutating the placement. Per-origin totals (arena, cache-mapped,
cache-unmapped, host) are counted separately so the log can print the **`sum == total`**
check, and cache samples fold onto their guest address so one guest location reads as one
row regardless of where it executed. The existing `REPIU_NATIVE_SAMPLING` sampler requires
a full second with no exception dispatch and therefore never fires in a stalled run; this
census uses no such gate. Runs with it enabled are not quotable for wall time or frames.

## AOT back-edge 타이머 safe point / AOT back-edge timer safe point

Task 348은 자연 VEH 경계가 없는 AOT busy-wait에서도 원본 INT 8 ISR을 계속 실행하기
위한 협력형 rendezvous를 추가합니다. `dynamic` emitter는 direct/conditional back edge
앞에 `pushfd`/request compare/`popfd` guard를 생성하고, 요청이 있을 때만 placement에
등록된 전용 `INT3`로 진입합니다. 정상 경로와 trap 경로 모두 원래 GPR/ESP/EFLAGS를
보존한 상태에서 기존 translated branch로 이어집니다.

```mermaid
sequenceDiagram
    participant P as Poll thread
    participant C as AOT cache
    participant V as Guest-thread VEH
    participant I as Common INT 8 injector
    P->>P: pending=true, request=1
    C->>V: back-edge safe-point INT3
    V->>V: request=0, EIP=ExceptionAddress+1
    V->>I: IF/vector 검증과 IRET frame 작성
    I-->>C: original ISR/IRETD 뒤 branch 재개
```

request word와 site index는 `Win32AotCodeCachePlacement`가 소유하며 initial placement와
dynamic append가 공유합니다. poll thread는 `InterlockedExchange`로 요청만 게시하고
guest context, TF, stack을 수정하지 않습니다. safe-point handler는 일반 AOT reentry보다
먼저 실행되며 실제 frame 작성은 기존 `InjectPendingInterrupts`만 담당합니다. 자연 경계에서
pending이 먼저 소비된 경우 request도 함께 지워 stale trap을 막습니다. 종료 로그는
site 수와 trap/injected/deferred를 분리해 기록합니다.

Task 348 adds a cooperative rendezvous that keeps the original INT 8 ISR running even inside
an AOT busy-wait with no natural VEH boundary. The `dynamic` emitter places a
`pushfd`/request-compare/`popfd` guard before direct and conditional back edges and enters a
placement-registered dedicated `INT3` only when requested. Both paths preserve the original
GPRs, ESP, and EFLAGS before continuing with the translated branch.

The request word and site index belong to `Win32AotCodeCachePlacement` and are shared by the
initial placement and dynamic appends. The poll thread publishes only the request with
`InterlockedExchange`; it never edits guest context, TF, or stack. The dedicated handler runs
before generic AOT reentry, explicitly resumes at `ExceptionAddress + 1`, and delegates all
frame creation to `InjectPendingInterrupts`. Natural-boundary delivery also clears the request
to prevent a stale trap. Final diagnostics report site and trap/injected/deferred counts.

### 타이머 IRQ0 in-service와 `sti` 시점 전달 / Timer IRQ0 in-service and delivery at `sti` (Task 736)

guest는 user mode에서 실행되므로 IF를 실제로 끌 수 없습니다. guest `cli`는 저장된 context를 고쳐
흉내 내지만 커널이 복귀 때 IF를 다시 켜므로, `InjectPendingInterrupts`의 IF 검사는 ISR 안의
`cli`를 보지 못합니다. 그래서 주입은 IF와 별도로 `PicTimerInService`(PIC의 IRQ0 in-service
비트)를 따릅니다. 주입이 비트를 세우고 port 0x20의 EOI(0x20, 0x60)가 지우며, 서 있는 동안의
시도는 보류됩니다. EOI를 쓰지 않는 핸들러를 위해 guest ESP가 주입 frame 위로 올라가면 지웁니다.
EOI 뒤 밀린 tick은 에뮬레이트된 `sti`가 세운 요청을 dispatcher의 다음 주입 시도가 소비하며
전달되고, 두 번 연속 `sti` 전달은 막아 중첩 깊이를 2로 묶습니다. 주입 frame의 EFLAGS에서는
엔진의 trace TF를 지웁니다. `REPIU_PIC_TIMER_IN_SERVICE=0`이면 이전 동작입니다. 자세한 것은
[Task 736 설계](docs/design/20260926-736-pic-timer-in-service.md)에 있습니다.

The guest runs in user mode and cannot really clear IF: a guest `cli` is emulated by editing the saved
context, and the kernel sets IF again on return, so the IF test in `InjectPendingInterrupts` cannot see
a `cli` inside an ISR. Injection therefore also follows `PicTimerInService`, the PIC's IRQ0 in-service
bit: set by an injection, cleared by an EOI written to port 0x20 (0x20 or 0x60), with attempts held
meanwhile; for a handler that never writes an EOI it is cleared once guest ESP rises above the injected
frame. After the EOI an owed tick is delivered when the dispatcher's next injection attempt consumes the
request an emulated `sti` sets, and two `sti` deliveries in a row are refused, bounding nesting at depth
2. The engine's trace TF is cleared from the injected frame's EFLAGS. `REPIU_PIC_TIMER_IN_SERVICE=0`
restores the earlier behaviour. See the
[Task 736 design](docs/design/20260926-736-pic-timer-in-service.md).

## AOT timer source 귀속 / AOT timer-source attribution

Task 351의 opt-in `REPIU_AOT_TIMER_SOURCE_PROFILE=1`은 Task 348 safe point를
원본 guest source별 interrupt-delivery 관측점으로 사용합니다. initial AOT placement와
dynamic append는 기존 breakpoint set 옆에 `breakpoint_offset -> guest_source` map을
유지합니다. handler는 정확한 source를 찾은 뒤에만 고정 크기 profile을 갱신합니다.

```mermaid
sequenceDiagram
    participant P as PIT scheduler
    participant L as Due-tick ledger
    participant S as AOT safe point
    participant I as Common injector
    participant R as Fixed source profile
    P->>L: 만료 tick saturating-add
    S->>I: pending INT 8 주입 시도
    alt 주입 성공
        I->>L: exchange(0)
        I-->>S: 소비 tick 수
        S->>R: source에 tick 귀속
    else 주입 보류
        I-->>S: 0
        S->>R: deferred만 기록
    end
```

기존 `last_timer_injection_ticks`는 누적 진단값이고, 새
`timer_interrupt_due_ticks`는 아직 어느 전달 경로에도 귀속되지 않은 만료 tick입니다.
공용 `InjectPendingInterrupts`가 모든 성공 경로에서 due ledger를 소비하고 그 수를
반환합니다. natural VEH 경계는 반환값을 기록하지 않아 오래된 tick을 다음 safe point에
잘못 붙이지 않습니다. 동시 scheduler add가 `exchange` 뒤에 발생하면 다음 주입을 위해
남으므로 tick을 잃지 않습니다.

profile은 VEH hot path에서 allocation과 lock을 피하기 위해 1,024-entry 고정 배열을
사용하며 source, trap/injected/deferred count, 귀속 tick, first/last global tick을
보존합니다. 종료 시에만 전체 entry를 tick 순으로 정렬해 attempt summary에 출력합니다.
이 기능은 기본적으로 꺼져 있고 원본 executable, ISR, IRET frame, tick 값과 cadence를
바꾸지 않습니다. 귀속 tick의 주기 환산은 해당 source가 정적으로 tick-wait임이 확인된
경우에만 pacing 상한으로 사용합니다.

Task 351's opt-in `REPIU_AOT_TIMER_SOURCE_PROFILE=1` reuses Task 348 safe
points as per-original-source interrupt-delivery observations. Initial AOT
placement and dynamic append maintain a
`breakpoint_offset -> guest_source` map beside the existing breakpoint set.
Only an exactly resolved source updates the fixed profile.

The existing `last_timer_injection_ticks` remains a cumulative diagnostic,
while `timer_interrupt_due_ticks` contains expirations not yet consumed by any
delivery path. Common `InjectPendingInterrupts` consumes that ledger on every
successful injection and returns the consumed count. Natural VEH boundaries
discard the return value, preventing stale ticks from being attached to a
later safe point. A scheduler add concurrent with the exchange remains for the
next injection.

The profile uses a fixed 1,024-entry array to avoid allocation and locks in the
VEH hot path, retaining source, trap/injected/deferred counts, attributed
ticks, and first/last global tick. Only final reporting sorts and prints all
entries. The feature is disabled by default and changes neither the original
executable, ISR, IRET frame, tick values, nor cadence. Period conversion is
treated as a pacing upper bound only for a source whose original disassembly
proves a tick-wait dependency.

## Glide ordinal 시간 귀속 / Glide ordinal time attribution

Task 353의 기본 OFF `REPIU_GLIDE_ORDINAL_TIME_PROFILE=1`은 decoded Glide gate
cycle과 기존 Task 333 rendezvous interval을 ordinal 0~255 고정 배열에 귀속합니다.
global `ExecutionTimeScope`는 optional completed-cycle output을 제공하며 ordinal
finalizer가 그 값을 재사용하므로 TSC를 추가로 읽지 않습니다. backend도 이미 보유한
enter/publish/host-start/host-finish/resume timestamp를 현재 bound ordinal에 직접
누적합니다.

```mermaid
flowchart LR
    G["decoded Glide gate"] --> S["global ExecutionTimeScope"]
    G --> B["bound ordinal"]
    B --> H["host command timestamps"]
    S --> P["fixed ordinal entry"]
    H --> P
    P --> F["final sorted snapshot"]
```

hot path는 allocation, lock, 정렬이 없고 종료 시에만 39개 활성 entry를 cycle 순으로
출력합니다. timeout에서 열린 command 하나는 global partial interval만 가질 수 있으므로
완료 count는 handled gate와 대조하고 global-minus-ordinal delta는 열린 gate가 하나일
때만 허용합니다. 최종 60초 A/B의 global Glide coverage는 평균 99.970%, observer
impact는 -0.67%였습니다.

`grBufferSwap`은 Glide gate의 50.21%, 현재 wall-clock의 약 17.32%이며 backend
interval의 99.09%가 host work입니다. `swap_interval=1`은 현재 backend에서 사용되지
않고 `SDL_GL_SwapWindow`가 호출됩니다. 다음 분해는 원본 swap 의미를 보존하면서 실제
swap interval과 present blocking을 관측해야 합니다.

Task 353's disabled-by-default `REPIU_GLIDE_ORDINAL_TIME_PROFILE=1` attributes
decoded Glide gate cycles and existing Task 333 rendezvous intervals to a
fixed direct-indexed ordinal array. The global `ExecutionTimeScope` exposes
its completed cycle count, so the ordinal finalizer adds no timestamp reads;
the backend likewise reuses its existing handoff timestamps. The hot path
allocates, locks, and sorts nothing, while final reporting orders all active
entries.

One command open at timeout may leave only global partial intervals; completed
counts are checked against handled gates and such deltas are accepted only
with one open gate. Final mean global coverage was 99.970% with -0.67%
observer impact. `grBufferSwap` owns 50.21% of the gate and about 17.32% of
wall time, with 99.09% of its backend interval in host work. Its received
`swap_interval=1` is currently unused before `SDL_GL_SwapWindow`, so the next
decomposition must measure actual swap state and present blocking without
changing original swap semantics.

## Glide buffer swap 시간 분해 / Glide buffer-swap time decomposition

Task 354의 기본 OFF `REPIU_GLIDE_SWAP_TIME_PROFILE=1`은 guest gate에서 온
`BufferSwap`만 setup, `SDL_GL_SwapWindow`, FPS accounting, finalize로 분해합니다.
LFB 경로가 host thread에서 직접 호출하는 internal present는 이 profile에서 제외합니다.
첫 profile swap에서 현재 context의 `SDL_GL_GetSwapInterval`을 한 번 관측하지만
`SDL_GL_SetSwapInterval`은 호출하지 않습니다.

```mermaid
flowchart LR
    G["guest grBufferSwap"] --> Q["host command"]
    Q --> S["setup + interval query"]
    S --> P["SDL_GL_SwapWindow"]
    P --> A["FPS accounting"]
    A --> F["finalize/result"]
    S --> T["fixed swap profile"]
    P --> T
    A --> T
    F --> T
```

최종 3×60초 Release profile의 내부 total은 ordinal 85 host-work를 평균 99.940%
덮었고, present가 내부 시간의 평균 99.589%였습니다. 4,030개 요청은 모두 interval
1이었고 실제 SDL interval도 세 실행 모두 1이었습니다. 현재 호스트에서 원본 요청과
실제 context가 일치하므로 성능을 위해 swap 동기화를 끄거나 프레임을 drop하지
않습니다. interval을 명시적으로 적용하는 변경은 다른 환경에서 불일치가 확인된 뒤
별도 fidelity 계약으로 검증합니다.

Task 354's disabled-by-default `REPIU_GLIDE_SWAP_TIME_PROFILE=1` decomposes
only guest-gate `BufferSwap` calls into setup, `SDL_GL_SwapWindow`, FPS
accounting, and finalize. Internal LFB presentation is excluded. The first
profiled swap observes `SDL_GL_GetSwapInterval` once without calling
`SDL_GL_SetSwapInterval`.

Across the final three 60-second Release runs, internal totals covered mean
99.940% of ordinal 85 host work and present averaged 99.589% of internal
time. All 4,030 requests used interval 1 and SDL reported interval 1 in every
run. The current host therefore matches the original request; synchronization
is not disabled and frames are not dropped for performance. Explicit interval
application requires a separate fidelity contract after a host mismatch is
observed.

## Glide setter 반복률과 GL phase 계측 / Glide setter census and GL phase instrumentation

Task 364는 기본 OFF 계측 두 개를 서로 다른 스레드에 나눠 둡니다. 반복률 census는
clock을 전혀 쓰지 않고, GL phase 분해만 새 timestamp를 만들기 때문에 환경 변수도
분리해 관측자 영향을 따로 판정합니다.

| 계측 | 환경 변수 | 소유자 | 스레드 | 추가 clock read |
|---|---|---|---|---:|
| setter 반복률 census | `REPIU_GLIDE_SETTER_CENSUS=1` | `ThreadContext` | guest | 0 |
| GL phase 분해 | `REPIU_GLIDE_SETTER_PHASE=1` | `GlideOpenGlBackend` | host | 호출당 3~4 |

census는 `HandleGlideGateBoundary` 한 곳에서 scope 객체로만 동작하며 setter dispatch
case를 수정하지 않습니다. gate stack mirror에서 고정 크기 key를 만들고, 변경하지 않은
dispatch가 끝난 뒤 `glide_gate_handled_count`와 implementation-issue 누적치 차이로
결과를 분류합니다. 따라서 census는 구조적으로 dispatch 결과를 바꿀 수 없습니다.

```mermaid
flowchart TD
    E["gate 진입"] --> K["고정 크기 key 캡처"]
    K --> D["변경 없는 dispatch"]
    D --> C{"handled 증가 & issue 증가 없음?"}
    C -->|"아니오"| V["applied 기록 무효화"]
    C -->|"예"| M{"applied key와 동일?"}
    M -->|"예"| S["same++"]
    M -->|"아니오"| G["changed++, applied 갱신"]
```

applied 기록은 "요청됨"이 아니라 **host에서 성공적으로 적용됨**을 뜻합니다. backend
실패와 retain된 unsupported 인수는 기록을 무효화하고, `grSstWinOpen`/`grSstWinClose`/
`grGlideInit`/`grGlideShutdown`/`grGlideSetState`/`grRenderBuffer`는 전체를
무효화합니다. texture 상태 key는 census 내부 monotonic generation을 포함하며 texture
download마다 증가하므로, 인수가 같아도 download를 가로질러 동일 판정이 나오지
않습니다. 이 규칙은 Task 365의 생략 규칙과 같아야 하며, 그래서 census가 재는 값이
실제 상한이 됩니다.

GL phase 분해는 timestamp 네 개로 `drain`/`apply`/`error`를 정확히 분할하므로
`drain + apply + error == total`이 항등식으로 성립합니다. clock은 backend가 이미 쓰는
`ReadGlideGateTimingCycles()`를 공유하고, `message_` 대입은 timed 구간 밖으로 옮겨
문자열 비용이 GL에 섞이지 않게 합니다.

census entry는 ordinal당 수백 바이트이고 배열이 256칸이므로 snapshot은 집계만
전달하고 per-ordinal 항목은 profile에서 직접 읽습니다. 값 복사 snapshot은 복사마다 약
100KB를 스택에 올립니다.

측정 entry point는 `scripts/task364_glide_setter_state_census.ps1`이며 Task 347 축을
control/profile로 각 3회 실행하고 항등식·overflow·관측자 gate를 검사합니다.

Task 364 splits two disabled-by-default instruments across the threads that own
them. The repetition census adds no clock reads at all, while only the GL phase
split creates new timestamps, so each carries its own opt-in and its own
observer verdict. The census operates purely as a scope object at the single
gate boundary and edits no setter dispatch case: it captures a fixed-size key
from the gate stack mirror and classifies the outcome after the unmodified
dispatch from the change in `glide_gate_handled_count` and the
implementation-issue totals, so it structurally cannot alter a result.

An applied record means state successfully applied on the host, not merely
requested. Backend failures and retained unsupported arguments void it; window
open/close, Glide init/shutdown, set-state, and render-buffer changes void every
record; and texture-state keys carry a census-local monotonic generation that
each texture download increments, so identical arguments do not compare equal
across a download. These are deliberately the same rules Task 365 must obey,
which is what makes the measured repetition rate a real ceiling.

Four timestamps partition the OpenGL interval so `drain + apply + error ==
total` holds by construction, the clock is the backend's existing
`ReadGlideGateTimingCycles()`, and the `message_` assignment sits outside the
timed region. Census snapshots carry aggregates only, with per-ordinal entries
read from the profile, because a by-value copy of the 256-entry array would put
about 100KB on the stack. The measurement entry point is
`scripts/task364_glide_setter_state_census.ps1`, which runs the Task 347 axis
three times each with the instruments off and on and checks the identities,
overflow counters, and observer gate.

## Timer tick 전달 회계 / Timer tick delivery accounting

Task 366은 guest가 프로그램한 timer tick과 실제로 전달된 tick의 차이를 상시 counter로
회계합니다. `PitIrqSchedule::Poll`은 밀린 tick 수를 정확히 돌려주지만
`timer_interrupt_pending`이 `std::atomic<bool>`이라 due가 몇이든 `INT 8`은 한 번만
전달됩니다. 실측 결손은 **11.9%** 입니다.

```mermaid
flowchart TD
    P["host poll loop"] --> S["PitIrqSchedule::Poll → due"]
    S --> R["RecordTimerTicksDue<br/>backlog += min(due, 64 - backlog)"]
    R --> B["timer_interrupt_pending = true"]
    B --> I["InjectPendingInterrupts"]
    I --> C{"safe point 조건<br/>IF, guest EIP"}
    C -->|"불충족"| DF["deferred (지연, 손실 아님)"]
    C -->|"충족"| J["INT 8 주입"]
    J --> K["RecordTimerTickInjected<br/>backlog가 남으면 armed 유지"]
```

밀린 tick은 상한 64의 backlog에 보존되고 safe point마다 하나씩 소진됩니다. counter는
`due`, `injected`, `dropped`, `deferred`, `due_in_gate`, `max_backlog`, `backlog`이며
**항등식 `due == injected + dropped + backlog`** 가 분해 경계의 근거입니다. `dropped`는
상한을 넘은 tick과 전달을 포기할 때 남아 있던 tick입니다. `deferred`는 지연이지 손실이
아니므로 항등식 밖입니다.

Task 366에서는 bool 하나로 전달해 밀린 tick을 버렸고(`coalesced`), backlog는 옵트인
실험이었습니다. 그때는 backlog가 상한에 붙어 safe point를 상시 무장시켜 프레임이 16.4%
떨어졌지만, 실행 속도가 오른 뒤 Task 432에서 backlog가 게이트 호출 사이에 비워짐을
확인하고(전달률 50.6% → 99.98%, 노트·BGA 튐 해소) 기본값으로 바꿨습니다. issue #24에서
bool 경로와 `REPIU_TIMER_TICK_BACKLOG` 스위치, 항상 0이 된 `coalesced` counter를
없앴습니다.

Task 366 accounts, with always-on counters, for the gap between the timer ticks the
guest programmed and the ones it received. Owed ticks are kept in a backlog capped at 64 and
drained one per safe point. The counters are owed, injected, dropped, deferred, owed inside the
Glide gate, peak backlog and outstanding backlog, and the identity
`due == injected + dropped + backlog` is what makes the decomposition trustworthy; `dropped` is
ticks past the cap or outstanding when delivery is abandoned, and deferrals sit outside the
identity because they are delays rather than losses.

Task 366 delivered through a single boolean that discarded owed ticks (`coalesced`) and kept
the backlog as an opt-in experiment, which then cost 16.4% of frames because the backlog pinned
at its cap and kept the safe point armed. Once execution got faster, Task 432 found the backlog
emptying between gate calls (delivery 50.6% to 99.98%, the note and BGA jumping gone) and made it
the default. Issue #24 removed the boolean path, the `REPIU_TIMER_TICK_BACKLOG` switch and the
`coalesced` counter that had become permanently zero.

## 동일 Glide 상태 생략 / Eliding already-applied Glide state

Task 365는 반복률 99.9% 이상인 7종 setter(`grColorMask`, `grAlphaBlendFunction`,
`grClipWindow`, `grAlphaTestFunction`, `grFogMode`, `grCullMode`,
`grDepthBufferFunction`)에서 **정확한 동일 상태의 host rendezvous만** 생략합니다.
이후 Task 439·443·444가 대상을 텍스처 상태와 batch 3·4로 넓혔고, 모두 항상 켜져
있습니다(끄기 스위치 `REPIU_GLIDE_SETTER_ELIDE`와 `_TEXTURE`·`_BATCH3`·`_BATCH4`는 #20에서
제거).

`glide_setter_state_cache`는 "요청됨"이 아니라 **host에서 성공적으로 적용됨**을
기록합니다. 규칙은 `glide_setter_state_model`에만 있고 census(관측자)와
cache(행위자)가 같은 함수를 쓰므로 둘이 어긋날 수 없습니다.

```mermaid
flowchart TD
    G["guest 호출"] --> X["AOT→HLE 예외 경계"]
    X --> V["반환 주소·signature·인수 크기 검증"]
    V --> M{"직전 성공 적용과 정확히 같은가?"}
    M -->|"아니오"| D["dispatch → backend → InvokeOnHostThread"]
    D --> S{"적용 성공?"}
    S -->|"예"| A["applied 기록"]
    S -->|"아니오"| Z["기록 무효화"]
    M -->|"예"| E["rendezvous만 생략"]
    A --> R["stdcall 정리 후 복귀"]
    Z --> R
    E --> R
```

유지되는 것은 예외 경계 진입, 세 가지 ABI 검증, `glide_gate_handled_count`,
stdcall 정리(`Esp += 4 + 인수 바이트`), 반환 주소, 호출 순서입니다. batch 1은 전부
void 반환이라 `Eax`는 건드리지 않습니다. host 소유 `backend.message_`는 생략 경로에서
쓰지 않습니다(guest thread에서 쓰면 경합).

`glide_state` mirror 쓰기를 건너뛰어도 안전한 근거는 **멱등성**입니다. 이 mirror는
`grGlideGetState`가 `BuildGlideStateImage`로 guest에 돌려주므로 실제로 읽히지만, key가
인수 dword 전체를 담으므로 직전 적용이 이미 동일한 값을 썼습니다.

측정 entry point는 `scripts/task365_glide_setter_state_elision.ps1`이며 OFF/ON 각
3회에 더해 짧은 시각 검증 pass를 수행합니다. 핵심 gate는 **census `same` ==
cache `elided`** 로, 동작을 바꾸지 않는 관측자가 센 중복과 실제 생략이 일치함을
증명합니다.

Task 365 elides only the host rendezvous, and only for an exact repeat of state
already applied successfully, across the seven setters Task 364 measured at 99.9%
or better repetition. Tasks 439, 443 and 444 later widened it to the texture state and
batches three and four, and all of it is always on (the `REPIU_GLIDE_SETTER_ELIDE` kill switch
and its `_TEXTURE`, `_BATCH3` and `_BATCH4` companions were removed in #20). The cache records successful host application rather
than a request, and its rules live solely in `glide_setter_state_model` so the
observing census and the acting cache cannot diverge.

Exception boundary entry, all three ABI validations, the handled count, the
stdcall cleanup, the return address, and call order are unchanged, and `Eax` is
untouched because batch one returns void throughout. The host-owned
`backend.message_` is never written from the guest thread. Skipping the
`glide_state` mirror write is safe by idempotence: it is genuinely read back
through `BuildGlideStateImage`, but the key holds every argument dword, so the
application this key matches already wrote identical values.

The measurement entry point is `scripts/task365_glide_setter_state_elision.ps1`,
which runs three samples per configuration plus a short visual pass. Its decisive
gate is `census same == cache elided`, proving the behaviour-neutral observer's
duplicate count equals what was actually skipped.

## Release 실행 축 측정 계약 / Release execution-axis measurement contract

Task 347의 `scripts/task347_release_axis_reattribution.ps1`은 runtime 의미를 바꾸지 않는
Release 측정 entry point입니다. 같은 seed의 격리 EEPROM 세 개, `dynamic`,
`REPIU_EXECUTION_TIME_PROFILE=1`, 60초 direct-loader timeout을 사용하고 실행별
stdout/stderr, JSON과 전체 CSV를 `build/benchmarks/release-axis/`에 남깁니다.

배타 예외 census는 `exception_dispatch_entry_count`와 비교하지 않습니다. 그 계수는
AOT write/timer/reentry/transfer early handler 뒤에서 시작하는 late-dispatch scope입니다.
전체 VEH 대조값은 함수 진입부의 `kVehTotal` profile count이며, timeout snapshot에서
열린 scope 한 건 때문에 census가 1 클 수 있습니다.

현재 Task 348/349 이후 Release 60초 3회 중앙값은 guest 실행 추정 60.72%, Glide gate
21.73%, VEH-exclusive 9.70%, 커널 전이 추정 6.83%입니다. 모든 single-step run이
길이 1입니다. Task 351은 그 guest 유도값 가운데 정적으로 확인된 240Hz timer pacing
상한을 9.83%p로 분리했고, 50.89%p를 active/unresolved 잔여로 남겼습니다.

active guest 주소 귀속에는 guest thread `SuspendThread` 표본과 “요청 뒤 처음 만나는
AOT back edge” 협력형 표본을 사용하지 않습니다. 전자는 syscall 경계와 정지 교란에,
후자는 first-eligible topology와 호출 빈도에 편향됨이 각각 확인됐습니다. 특히 짧은
arm-to-hit latency와 실행 간 안정된 순위는 instruction residency의 충분조건이
아닙니다. 향후 이 축은 정확한 executed-edge/instruction-count 계측이나 외부
PMU처럼 이 두 편향을 피하는 방법에서만 다시 엽니다.

Task 347's `scripts/task347_release_axis_reattribution.ps1` is a Release
measurement entry point that changes no runtime semantics. It uses three
EEPROM copies from one seed, `dynamic`,
`REPIU_EXECUTION_TIME_PROFILE=1`, and a 60-second direct-loader timeout,
then writes per-run logs and JSON plus aggregate CSV under
`build/benchmarks/release-axis/`.

The exclusive exception census is not compared with
`exception_dispatch_entry_count`, whose scope begins after AOT write, timer,
re-entry, and transfer early handlers. The whole-VEH reference is the
function-entry `kVehTotal` profile count; one open scope at the timeout snapshot
can leave the census one count higher.

On the current post-Task-348/349 build, three Release runs put median estimated
guest execution at 60.72%, the Glide gate at 21.73%, VEH-exclusive work at
9.70%, and estimated kernel transitions at 6.83%. Every single-step run has
length one. Task 351 separated a statically confirmed 9.83-percentage-point
upper bound for 240 Hz timer pacing from that derived guest share, leaving
50.89 percentage points as active/unresolved guest time.

Active guest address attribution must not use either cross-thread
`SuspendThread` sampling or cooperative "first AOT back edge after request"
sampling. The former is biased toward syscall boundaries and perturbs the
target through suspension; the latter is biased toward first-eligible
topology and call frequency. Short arm-to-hit latency and stable rankings are
not sufficient evidence of instruction residency. This axis may be reopened
only with exact executed-edge/instruction-count instrumentation or an
external PMU-class method that avoids both biases.

## AOT-DBT Glide gate direct dispatch / AOT-DBT Glide 게이트 직접 디스패치

direct 모델(Win32, Linux i386)의 `dynamic`는 `REPIU_AOT_DBT_GLIDE_GATE_DISPATCH`가 미설정이거나 `1|on|true`이면 자산 유래 Glide gate metadata와 합성 stub 원본을 검증한 뒤 `CALL host-stack thunk + RET argument_bytes` stub을 설치합니다. 첫 cache boundary는 같은 gate를 가리키는 direct fixup을 executable LINEXE gate로 재연결하며(Task 773: gate별 fixup 색인 `AotGlideGateFixupIndex`로 찾고, 이미 그 값을 담은 slot은 다시 쓰지 않으며, direct 모델에서는 rel32를 2³²로 감아 캐시와 gate의 거리에 상관없이 잇습니다. 문서에 있던 indirect inline-cache target 재연결은 동작한 적이 없어 제거했습니다), 이후 transfer resolution도 검증된 gate를 직접 반환합니다. 일반 excluded range, opt-out, 검증 실패는 기존 `INT3`/VEH 경로를 보존합니다.

On the direct model (Win32, Linux i386) under `dynamic`, an unset `REPIU_AOT_DBT_GLIDE_GATE_DISPATCH` or `1|on|true` validates asset-derived Glide metadata and the original synthetic stub, then installs a `CALL host-stack thunk + RET argument_bytes` stub. The first cache boundary relinks matching direct fixups to the executable LINEXE gate (Task 773: found through the per-gate fixup index `AotGlideGateFixupIndex`, slots already holding the value are not rewritten, and on the direct model the rel32 wraps at 2³² so the link is made whatever the distance between the cache and the gate; the indirect inline-cache target relink this document described never worked and was removed), and later transfer resolution returns validated gates directly. General excluded ranges, opt-out, and validation failures preserve the existing `INT3`/VEH path.

direct-dispatch 설정은 정책 요청이며 capability 보장이 아닙니다. loader와
LINEXE setup은 32비트 guest image에 표현 가능한 host thunk가 실제로
제공되는지 별도로 확인합니다. Linux x64처럼 thunk가 없는 host에서는 실제
direct-dispatch를 끄고 검증된 base HLE gate image를 유지하여 LINEXE 활성화와
기존 trap/HLE 경계를 계속 사용할 수 있게 합니다.

The direct-dispatch setting is a policy request, not a capability guarantee.
The loader and LINEXE setup separately test whether the host supplies a thunk
that can be represented in the 32-bit guest image. On Linux x64 and other hosts
without that thunk, actual direct dispatch is false and the validated base HLE
gate image remains installed, allowing LINEXE activation and the existing
trap/HLE boundary to continue.

### swap 대기 중 타이머 tick / Timer ticks during the swap wait

`grBufferSwap` gate는 present를 호스트 스레드에 게시하고 기다리는 동안 밀린 타이머 tick을 "gate에 도달한 `call` 직전의 인터럽트"로 주입합니다(Task 750). cache 모델은 대기 시작부터 주입합니다. direct 모델(#6)은 gate thunk가 게스트 스택에 둔 프레임과 인터럽트 프레임이 겹치므로, resolver가 thunk의 끝을 엔진이 만든 출구 코드(`GlideGateInterruptExit`: `push cs; push [eip]; jmp [handler]`)로 보내 핸들러에 들어갑니다. direct 모델은 대기가 50 ms를 넘긴 뒤에만 주입하므로(`REPIU_GLIDE_SWAP_WAIT_TICK_HOLD_MS`) 보통의 swap은 이전과 같고, 숨겨진 창처럼 swap이 오래 막힐 때 tick이 버려지지 않습니다. `REPIU_GLIDE_SWAP_WAIT_TICKS=0`으로 끕니다.

While the `grBufferSwap` gate waits for the present it posted to the host thread, owed timer ticks are injected as "an interrupt just before the `call` that reached the gate" (Task 750). The cache model injects from the start of the wait. On the direct model (#6) the interrupt frame overlaps the frame the gate thunk keeps on the guest stack, so the resolver sends the thunk's tail through exit code the engine generates (`GlideGateInterruptExit`: `push cs; push [eip]; jmp [handler]`) into the handler. The direct model injects only once the wait has lasted 50 ms (`REPIU_GLIDE_SWAP_WAIT_TICK_HOLD_MS`), so an ordinary swap is as before and a swap that stays blocked, as for a hidden window, no longer loses ticks. `REPIU_GLIDE_SWAP_WAIT_TICKS=0` switches it off.

### Glide gate 직접 dispatch 기본 정책 / Glide-gate direct-dispatch default policy

`0|off|false`, 빈 문자열, 알 수 없는 값은 fail-closed opt-out입니다. 자산 유래 gate/ABI 검증 실패와 `dynamic` 이외 backend는 기존 UD2/INT3/VEH 경로를 유지합니다.

`0|off|false`, an empty string, and unknown values are fail-closed opt-outs. Asset-derived gate/ABI validation failures and backends other than `dynamic` retain the existing UD2/INT3/VEH path.

## Guarded segment-load fast path / Guarded segment-load fast path

Win32 `dynamic`에서는 register-source `MOV Sreg, r16` 중 ES/DS/FS/GS를 전용 cache slot으로 처리합니다. source selector가 실제 CPU selector와 HLE shadow에 모두 같은 경우만 selector 상태를 바꾸지 않고 fallthrough합니다. SS, ESP source, memory source, selector 불일치, patch 실패는 원래 EFLAGS/GPR을 복구한 뒤 기존 INT3/VEH HLE를 사용합니다. 끄기 스위치 `REPIU_AOT_GUARDED_SEGMENT_LOAD`(와 같은 형태의 `_READ`)는 issue #30에서 없앴습니다.

On Win32 `dynamic`, the path handles register-source `MOV Sreg, r16` for ES/DS/FS/GS in a dedicated cache slot. It leaves selector state unchanged and falls through only when the source selector equals both the physical CPU selector and HLE shadow. SS, ESP sources, memory sources, selector mismatches, and patch failures restore original EFLAGS/GPRs and retain the existing INT3/VEH HLE path. The kill switch `REPIU_AOT_GUARDED_SEGMENT_LOAD` (and the matching `_READ`) went in issue #30.

## Hybrid segment-override dispatch (삭제됨) / Hybrid segment-override dispatch (deleted)

Task 391·392는 `kSegmentOverrideMem`에 selector-guard native slot과 fail-closed HLE
companion slot을 함께 만들어, live segment policy가 `HleLowMemory`이면 companion으로
`JMP rel32`를 패치하는 옵트인(`REPIU_AOT_DBT_SEGMENT_OVERRIDE_DISPATCH`)을 더했습니다.
`pumpit1` 장시간 검증에서 frame 처리량이 21.13% 낮아 기본 OFF로 남았고, issue #22에서
companion slot 방출, 검증, 패치와 함께 삭제했습니다. slot은 `je 0x02; popfd; int3`이고,
일반 실행은 selector-guard native folding과 low-memory INT3/VEH HLE 경로를 씁니다.

Tasks 391 and 392 added an opt-in (`REPIU_AOT_DBT_SEGMENT_OVERRIDE_DISPATCH`) that emitted a
fail-closed HLE companion beside the selector-guard native slot of a `kSegmentOverrideMem`
and patched a `JMP rel32` to it when the live segment policy was `HleLowMemory`. Long
`pumpit1` validation delivered 21.13% fewer frames, so it stayed default off; issue #22
deleted it with the companion emission, validation and patching. The slot is
`je 0x02; popfd; int3`, and execution uses selector-guard native folding and the low-memory
INT3/VEH HLE path.

## Port I/O 주소 census와 arena 진입 추적 / Port I/O address census and arena entry tracing

Tasks 405~409은 port I/O 예외의 위치와 원인을 귀속하기 위한 계측 묶음을 추가합니다.
`HandlePortIoInstruction` 한 곳에서만 기록하며 동작을 바꾸지 않습니다.

```mermaid
flowchart TD
    V["VEH 관문<br/>RecordVehExceptionCensus"] --> H["last_veh → prev_veh 한 칸 이동"]
    H --> P["HandlePortIoInstruction"]
    P --> A["주소별 census<br/>count / cache / mapped / reentry"]
    P --> E{"arena fault이고<br/>직전이 0xC0000096이 아님"}
    E -->|"예"| S["진입 표본 1건 + 4분류 히스토그램"]
    E -->|"예"| R["전역 ring 최신 16건"]
```

| 항목 | 기본값 | 내용 |
|---|---|---|
| 주소별 count/cache | 상시 ON | 32칸 선형 표. `cache`는 AOT 캐시 안에서 실행됐는지 |
| `mapped`/`reentry` | `REPIU_PORT_IO_CENSUS_MAPPING` (OFF) | `FindAotCacheAddress` 조회가 붙어 약 5.8% 느려집니다. **켠 실행의 wall·프레임은 인용 금지** |
| 진입 표본 1건 + 히스토그램 | 상시 ON | 주소별 첫 전이와 직전 예외 4분류 |
| 전역 진입 ring | 상시 ON | 최신 16건. 주소별 표본과 판정식을 공유합니다 |

**주소별과 전역을 함께 두는 이유:** 전역 버퍼는 가장 시끄러운 주소가 차지하므로 특정
주소를 겨냥할 수 없고(Task 407), 주소별 표본은 "마지막에 무슨 일이 있었나"를 답하지
못합니다. 서로 대체하지 않습니다.

**계측 주의:** profiled `kPortIoDevice` count와 cycles는 port I/O를 과대 계상합니다.
`ExecutionTimeScope`가 함수 진입 시 생성되어 opcode 검사에서 빠져나가는 호출까지 세기
때문이며, 차이는 single-step 횟수를 따라갑니다. **실제 횟수는 census 쪽입니다.**

Tasks 405-409 add one instrumentation cluster that attributes where port I/O exceptions occur
and why, recorded at a single site in `HandlePortIoInstruction` with no behaviour change. The
per-address census, its first entry sample, the four-way predecessor histogram, and the global
entry ring are always on; only the `mapped`/`reentry` fields are opt-in under
`REPIU_PORT_IO_CENSUS_MAPPING`, because their `FindAotCacheAddress` lookup costs about 5.8% and
would distort the measurement it serves — **wall time and frame counts from enabled runs are
not quotable**.

Per-address and global buffers coexist deliberately: a global buffer is taken by the noisiest
address and cannot target a specific one (Task 407), while a per-address sample cannot say what
happened most recently. They share one transition test so they can never disagree about what an
entry is.

**Measurement caveat:** the profiled `kPortIoDevice` count and cycles over-count port I/O,
because `ExecutionTimeScope` is constructed on entry and so counts calls that bail at the
opcode check; the gap tracks the single-step count. **The census is the accurate count.**

## 지속적 통합과 릴리스 아티팩트 / Continuous integration and release artifacts

CI는 GitHub Actions 호스티드 러너에서만 돌고, 워크플로는 둘입니다
([Task 434](docs/design/20260806-434-github-actions-release-ci.md)).

| 워크플로 | 트리거 | 하는 일 |
|---|---|---|
| `.github/workflows/ci.yml` | `main` push, pull request | Win32 **Debug** 빌드 + probe 2종 |
| `.github/workflows/release.yml` | `v*` 태그, 수동 실행 | 버전 게이트 → Win32 **Release** 빌드 → probe 2종 → OpenWatcom 819샘플 → zip·리포트, 나란히 Linux i386·x64 **Release** 빌드 → SDL 드라이버·glibc 확인 → probe 2종 → tar.gz (Task 767) → 모두 성공하면 `publish` job이 아티팩트 4종을 Release에 첨부 |

**검증 범위의 경계가 이 설계의 핵심입니다.** `roms/`와 `MASTER/`는 저작물이라 CI에
없으므로 **pumpit1·pumpit3 실행 검증과 모든 성능 수치는 로컬 전용**입니다. CI가 볼 수
있는 것은 빌드, probe 두 개, 그리고 OpenWatcom 샘플 회귀입니다.

* `repiu_aot_probe`는 CI에서 **`--timer-safe-point` 모드로만** 부릅니다. 전체 단정
  묶음은 `argv[1]`로 DOS4GW 이미지를 받고 `PIU.EXE`를 전제로 하며, CI가 만들 수 있는
  어떤 이미지로도 캐시 방출 단계를 통과하지 못합니다.
* 샘플 스위트는 **Release 로더로** 돌립니다. 배포하는 바이너리를 그대로 검증하기
  위해서입니다. `test_openwatcom_samples.ps1 -Configuration`의 기본값은 Debug이며,
  로컬 절차와 기존 기준선의 의미는 그대로입니다.
* 샘플마다 **기본 10초**의 하네스 시간 상한이 있고, 넘기면 프로세스를 kill하고
  **실패로 집계**합니다. 로더의 1,000 ms는 게스트 실행 예산이라 호스트 쪽 블록을 막지
  못합니다 — 표준 입력을 읽는 샘플에서 실제로 39.4분·CPU 1.0초의 정지가 관측됐습니다.
  stdin은 빈 파일로 redirect합니다.
* 통과 판정은 timeout을 실패로 세므로 **구성이 다르면 비교가 성립하지 않습니다.**
  summary와 baseline에 `RunCriterion`과 `Configuration`을 함께 기록하고,
  `-CompareBaseline`이 어느 한쪽이라도 다르면 경고합니다.
* 러너 이미지는 `windows-2022`로 **고정**합니다. `build_win32_x86.ps1`이 설치된 Visual
  Studio major 버전으로 생성기를 고르므로, 이미지가 떠다니면 릴리스 산출물의 툴체인이
  조용히 바뀝니다.

아티팩트는 `scripts/package_release.ps1`이 만들고 두 개입니다 —
`rePIU-v<version>-win32.zip`(정적 링크된 실행 파일과 고지 문서)과
`openwatcom-samples-v<version>.zip`(샘플 리포트). **회귀로 job이 실패해도 리포트는
업로드**하며, 반대로 **리포트 없이는 릴리스를 발행하지 않습니다.**

Linux 아카이브 두 개(`rePIU-v<version>-linux-i386.tar.gz`, `…-linux-x64.tar.gz`)는
`scripts/package_release_linux.sh`가 만듭니다. `ubuntu-22.04`에서 `REPIU_STATIC_RUNTIME`
(`-static-libstdc++ -static-libgcc`)으로 빌드하므로, 동적 의존성은 `libGL`과 glibc 2.35 이상뿐입니다.
SDL은 X11, Wayland, libdecor, 사운드 라이브러리를 실행 중에 엽니다. 이 드라이버들이 빠진 채 configure되면
job이 실패합니다. 게시는 `publish` job 한 곳에서만 하며, 플랫폼 하나라도 실패하면 Release를 만들지 않습니다
([Task 767](docs/design/20261003-767-linux-release-artifacts.md)).

CI runs only on GitHub-hosted runners, through two workflows
([Task 434](docs/design/20260806-434-github-actions-release-ci.md)): `ci.yml` builds Win32
**Debug** and runs two probes on pushes to `main` and on pull requests, while `release.yml`
runs on `v*` tags with a version gate, a Win32 **Release** build, the same probes, the 819
OpenWatcom samples, and two artifacts attached to the release.

**The verification boundary is the point of the design.** `roms/` and `MASTER/` are
copyrighted and absent from CI, so **pumpit1 and pumpit3 execution checks and every
performance figure stay local**; CI sees the build, the two probes, and sample regression.
`repiu_aot_probe` is invoked **only as `--timer-safe-point`**, because its full assertion set
takes a DOS4GW image as `argv[1]`, presumes `PIU.EXE`, and clears the cache-emission stage on
no image CI can build. Each sample carries a **10-second harness time bound**, past which the
process is killed and **scored as a failure**: the loader's own 1,000 ms budget governs guest
execution and cannot catch a host-side block, one of which was measured at 39.4 minutes on 1.0
second of CPU in a sample reading standard input, which is now redirected from an empty file.
The sample suite runs **against the Release loader** so the shipped
binary is the tested one, while `-Configuration` defaults to Debug locally so existing
procedures and the recorded baseline keep their meaning. Because the pass criterion counts a
timeout as a failure, a run is not comparable across configurations, so `RunCriterion` and
`Configuration` are both recorded in the summary and the baseline and `-CompareBaseline` warns
on either mismatch. The runner image is **pinned** to `windows-2022`, since
`build_win32_x86.ps1` selects its generator from the installed Visual Studio major version.

`scripts/package_release.ps1` produces both artifacts — `rePIU-v<version>-win32.zip` with the
statically linked executables and the notices, and `openwatcom-samples-v<version>.zip` with the
sample report. **The report uploads even when regressions fail the job**, and conversely **no
release is published without it.**

The two Linux archives (`rePIU-v<version>-linux-i386.tar.gz`, `…-linux-x64.tar.gz`) come from
`scripts/package_release_linux.sh`. They are built on `ubuntu-22.04` with `REPIU_STATIC_RUNTIME`
(`-static-libstdc++ -static-libgcc`), so their only dynamic dependencies are `libGL` and glibc
2.35 or newer; SDL opens X11, Wayland, libdecor and the sound libraries at run time, and the job
fails if it was configured without those drivers. Publishing happens once, in the `publish` job,
and no release is created unless every platform succeeded
([Task 767](docs/design/20261003-767-linux-release-artifacts.md)).
## Dynamic DOS 서비스 전달 / Dynamic DOS service routing

Win32 `dynamic` backend는 `enable_dos_hle=false`, `enable_traced_dos_hle=true`로 실행되므로
software interrupt가 먼저 `HandleTracedDosInterrupt21()`을 통과합니다. 이 함수는 안전하게
추적할 수 있는 서비스를 선별하지만, 서비스 의미를 다시 구현하지 않습니다. 공용 DOS
처리기가 소유하는 서비스는 그곳으로 위임해야 하며, `AH=3Ch` 파일 생성도
`HandleDosInterrupt21()`로 전달됩니다. 공용 처리기는 계수, VFS 상태 변경, DOS flags와
register 반환, EIP 진행을 한 번만 수행합니다.

On Win32, the `dynamic` backend runs with `enable_dos_hle=false` and
`enable_traced_dos_hle=true`, so software interrupts pass through
`HandleTracedDosInterrupt21()` first. That function selects services safe for
traced execution but does not reimplement their semantics. Services owned by
the common DOS handler must delegate to it; `AH=3Ch` file creation routes to
`HandleDosInterrupt21()`. The common handler remains the single owner of
accounting, VFS mutation, DOS flag/register results, and EIP advancement.
## Viewport 비례 Glide point raster / Viewport-scaled Glide point raster

Glide guest 좌표는 원본 논리 surface(관측된 PIU는 640×480)에 유지되고 OpenGL viewport가
실제 drawable로 확대합니다. 정점 위치와 달리 `GL_POINTS` raster 크기는 viewport로
확대되지 않으므로 backend는 drawable/logical 가로·세로 비율 중 작은 값을 point 크기로
사용합니다. 최소 1 physical pixel과 `GL_ALIASED_POINT_SIZE_RANGE`를 지키며,
`ApplyDrawableViewport()`가 resize 때 값을 갱신하고 직접·batch draw의 공용
`PrepareDrawState(GL_POINTS)`가 적용합니다.

Glide guest coordinates remain on the original logical surface (640×480 for
the observed PIU targets), while the OpenGL viewport expands them to the actual
drawable. Unlike vertex positions, the `GL_POINTS` raster size does not scale
with the viewport. The backend therefore uses the smaller drawable/logical axis
ratio as point size, floors it at one physical pixel, and clamps it to
`GL_ALIASED_POINT_SIZE_RANGE`. `ApplyDrawableViewport()` refreshes the value on
resize and the common direct/batched `PrepareDrawState(GL_POINTS)` applies it.
## 창 키보드 BIOS 입력 / Window keyboard BIOS input

Task 496부터 SDL 게임 창의 key down/up event는 기존 JAMMA 경로와 별개로 BIOS 키보드
HLE에도 전달됩니다. 플랫폼 공용 `hle::BiosKeyboard`는 실제 BIOS data area ring과 같은
15-entry FIFO, legacy/enhanced AX 쌍, shift/lock 상태를 소유합니다. SDL host thread가
producer이고 guest `INT 16h` thread가 consumer이므로 짧은 mutex 임계 구역으로 보호합니다.
가득 찬 FIFO는 새 입력을 버려 기존 순서를 보존합니다.

Win32 SDL adapter는 US 101-key scan code, ASCII, Shift/Caps/Ctrl/Alt 조합,
function/navigation 및 numeric keypad를 변환합니다. key repeat는 DOS typematic keystroke로
enqueue하고 key up은 modifier 상태만 갱신합니다. 포커스를 잃으면 눌림 modifier를 해제하고
lock toggle은 유지합니다. `AH=01h/11h`는 head를 소비하지 않고 ZF로 유무를 알리며
`AH=00h/10h`가 같은 head를 소비합니다. `AH=02h/12h`는 현재 shift flags를 반환합니다.
원본 실행 파일과 게임 로직은 수정하지 않습니다.

```mermaid
flowchart LR
    E["SDL keyboard event"] --> J["existing JAMMA mapping"]
    E --> A["SDL BIOS adapter"]
    A --> Q["BiosKeyboard FIFO + flags"]
    Q --> I["INT 16h HLE"]
    I --> G["original game keyboard poll"]
```

Since Task 496, SDL game-window key down/up events also feed the BIOS keyboard HLE independently
of the existing JAMMA path. Platform-neutral `hle::BiosKeyboard` owns a 15-entry FIFO matching the
usable capacity of the BIOS data-area ring, paired legacy/enhanced AX values, and shift/lock state.
Short mutex critical sections protect the SDL host-thread producer and guest `INT 16h` consumer.
A full FIFO drops the new input and preserves existing order.

The Win32 SDL adapter translates US 101-key scan codes and ASCII, modifier combinations,
function/navigation keys, and the numeric keypad. Key repeat enqueues DOS-style typematic
keystrokes; key-up only updates modifier state. Focus loss releases pressed modifiers while
retaining lock toggles. `AH=01h/11h` peeks without consuming and reports presence through ZF,
`AH=00h/10h` consumes that head, and `AH=02h/12h` returns current shift flags. No executable or
game logic is modified.

---

## Linux x64 LFB native-store source census

Linux x64에서는 `REPIU_LINUX_X64_LFB_STORE_SOURCE_CENSUS=1|on|true`가 독립적으로
native write observer와 write-LFB active-range gate를 활성화합니다. guest EIP별 표본은
`ThreadContext` 밖의 고정 64-entry 상태에 보관하며, positive overlap 4,096건마다 하나만
기록합니다. 종료 snapshot은 동률 EIP를 오름차순으로 정렬한 상위 8개를 보고합니다.

On Linux x64, `REPIU_LINUX_X64_LFB_STORE_SOURCE_CENSUS=1|on|true` independently enables
the native write observer and the write-LFB active-range gate. Per-guest-EIP samples live in
fixed 64-entry state outside `ThreadContext` and record one of every 4,096 positive overlaps.
The shutdown snapshot reports the top eight, breaking equal counts by ascending EIP.

---

## LFB staging shadow / LFB staging shadow

Tasks 728·729는 write `grLfbLock`이 매번 수행하던 full framebuffer readback과 565 encode를,
host가 그 buffer의 내용을 이미 들고 있을 때 건너뛰게 합니다. `GlideLfbStagingShadowState`는
**color buffer마다 하나씩**(front=0, back=1) host 소유 565 복사본을 둡니다.

* 성공한 write unlock(`flip_v=false`)은 staging surface를 `buffers[lock_buffer]`에 복사합니다.
* **`grBufferSwap`은 두 복사본을 교환합니다.** Glide의 buffer swap은 page flip이므로 swap 뒤
  back buffer는 직전 front buffer의 내용을 담습니다. 이것이 핵심입니다.
* draw·clear는 현재 render target(`grRenderBuffer`로 추적, 기본 back)의 복사본만 무효화합니다.
* region gate와 미분류 gate는 두 복사본을 모두 무효화합니다. 무효화 술어는 **기본값이
  무효화**인 allowlist입니다.
* lock은 복사본이 valid하고 format·해상도가 맞으면 readback 대신 memcpy합니다. 복사본은
  staging surface 자체가 아니므로 lock이 surface를 guest에 넘겨도 살아남습니다.

Task 728의 단일 shadow(staging surface 자체)는 측정에서 재사용 0건이었습니다. 무효화 304건이
전부 swap이었기 때문입니다. Task 729는 unlock과 다음 lock 사이 구간을 재어 303/303이 **정확히
swap 1회뿐**임을 확인했고, 그래서 swap을 손실이 아니라 교환으로 모델링했습니다.

`REPIU_GLIDE_LFB_STAGING_SHADOW_CENSUS=1|on|true`는 계수만, `REPIU_GLIDE_LFB_STAGING_REUSE=1|on|true`는
실제 적용입니다. **둘 다 기본 OFF입니다.** apply 모드에서 관찰된 teardown segfault는 같은 설정의 OFF
실행에서도 같은 모양으로 재현되어 이 기능과 무관한 기존 결함으로 확인됐습니다(Task 729 후속).
기본값 전환은 OFF 대 OFF 장면 기준선과 성능 측정 뒤에 검토합니다. 별도로
`REPIU_GLIDE_LFB_LOCK_INTERVAL_CENSUS=1|on|true`는 unlock과 다음 lock 사이 구간을 swap만 있음 /
clear / draw / region으로 분류합니다.

`ReadbackFramebuffer`는 window drawable의 content rect(Task 769)를 읽어 논리 해상도로 nearest-neighbor 축소하고
`PresentLfbSurface`는 다시 확대하므로, drawable이 정수배가 아니면 present → readback 왕복은
원래 pixel을 복원하지 못합니다. 또 OpenGL은 swap 뒤 back buffer 내용을 정의하지 않습니다(issue #69부터는
offscreen pair가 교환되므로 swap 뒤 back은 직전 front로 정의됩니다).
복사본 재사용은 이 두 불확실성을 원본 하드웨어의 page flip 의미로 대체합니다.

Tasks 728 and 729 let a write `grLfbLock` skip the full framebuffer readback and 565 encode it
used to run every time, when the host already holds that buffer's contents.
`GlideLfbStagingShadowState` keeps **one host-owned 565 copy per color buffer** (front 0, back 1).

* A successful write unlock (`flip_v=false`) copies the staging surface into
  `buffers[lock_buffer]`.
* **`grBufferSwap` exchanges the two copies.** A Glide buffer swap is a page flip, so the
  post-swap back buffer holds what the front buffer held. This is the crux.
* Draws and clears invalidate only the copy of the current render target, tracked from
  `grRenderBuffer` and defaulting to back.
* Region gates and unclassified gates invalidate both copies. The predicate is an allowlist whose
  **default is to invalidate**.
* A lock memcpys from a valid copy of matching format and resolution instead of reading back.
  The copy is not the staging surface itself, so it survives the lock handing that surface to
  the guest.

Task 728's single shadow, which was the staging surface itself, measured zero reuse because all
304 invalidations were swaps. Task 729 measured the window between an unlock and the next lock,
found 303 of 303 held **exactly one swap and nothing else**, and so models the swap as an
exchange rather than a loss.

`REPIU_GLIDE_LFB_STAGING_SHADOW_CENSUS=1|on|true` counts only; `REPIU_GLIDE_LFB_STAGING_REUSE=1|on|true`
applies. **Both are off by default.** The teardown segfault seen in apply mode reproduced with the
same signature in an OFF run under the same settings, establishing it as a pre-existing defect
unrelated to this feature (Task 729 follow-up). Changing the default waits for an OFF-against-OFF
scene baseline and a performance measurement. Separately,
`REPIU_GLIDE_LFB_LOCK_INTERVAL_CENSUS=1|on|true` classifies the window between an unlock and the
next lock as swaps-only, cleared, drawn, or region.

`ReadbackFramebuffer` reads the window drawable's content rect (Task 769) and nearest-neighbor downsamples it to the logical
resolution while `PresentLfbSurface` upscales it again, so on a drawable that is not an integer
multiple the present-then-readback round trip does not restore the original pixel; and OpenGL
leaves back buffer contents undefined after a swap (since issue #69 the offscreen pair is exchanged,
so after a swap back is defined as the previous front). Reusing the copies replaces both
uncertainties with the page-flip semantics of the original hardware.

---

## x64 종료 회수 판정 / x64 shutdown recovery decision

Task 730부터 예산 만료·종료 요청 시 guest thread 회수 여부는 `DecideShutdownRecovery`
(`include/repiu/engine/shutdown_recovery_policy.h`)가 정합니다. x64 Linux에서는 시그널 context의
**전체 RIP**를 읽어, 상위 32비트가 0이고 하위 32비트가 guest 이미지나 AOT cache 안일 때만 cache 탈출
trampoline으로 회수합니다. guest 코드와 cache는 설계상 4 GiB 아래에 놓이므로 이 조건은 정당한 회수를
막지 않습니다. 32비트 host는 이전과 같습니다. 결과는 `[repiu-shutdown]` 줄의 `host_ip=`,
`decision=`, `aliased=`로 보고됩니다.

From Task 730, whether the guest thread is recovered at budget expiry or on a quit request is decided
by `DecideShutdownRecovery` (`include/repiu/engine/shutdown_recovery_policy.h`). On x64 Linux it reads
the **full RIP** from the signal context and recovers through the cache-exit trampoline only when the
upper 32 bits are zero and the low 32 bits are in the guest image or AOT cache. Guest code and the
cache are placed below 4 GiB by design, so the condition never blocks a legitimate recovery. 32-bit
hosts are unchanged. The outcome is reported as `host_ip=`, `decision=` and `aliased=` on the
`[repiu-shutdown]` line.

Task 733부터 Win32에는 종료 회수 구간 동안만 **redirect guard**가 있습니다. Win32 회수는 guest thread를
멈추고 `SetThreadContext`로 EIP·ESP를 바꾸는데, legacy backend에서는 멈춘 순간 커널이 이미 single-step
예외를 전달하던 중일 수 있고 재작성은 그 예외를 취소하지 않습니다. guard는 엔진 VEH보다 먼저 호출되는
VEH로, 회수 적용 뒤 guest thread에 도착한 예외 중 context EIP가 회수 진입점이거나 guest/cache 코드인 것에
`RecoverToHost`를 다시 적용하고 실행을 재개합니다(`DecideShutdownRedirectGuard`). guest thread가 멈춘 뒤
제거되며 `[repiu-shutdown] redirect-guard reapplied_entry=N reapplied_guest=M`을 남깁니다. 배경은
[Win32 thread context 재작성과 전달 중인 예외](docs/kb/win32-thread-context-rewrite-and-in-flight-exceptions.md)에
있습니다.

From Task 733 Win32 has a **redirect guard** for the shutdown recovery window only. Win32 recovery suspends
the guest thread and rewrites EIP and ESP with `SetThreadContext`; under the legacy backend the kernel may
already be delivering a single-step exception at that moment, and the rewrite does not cancel it. The guard
is a VEH called before the engine's: for exceptions reaching the guest thread after the redirect whose
context EIP is the recovery entry or guest/cache code, it reapplies `RecoverToHost` and resumes
(`DecideShutdownRedirectGuard`). It is removed after the guest thread stops and reports
`[repiu-shutdown] redirect-guard reapplied_entry=N reapplied_guest=M`. Background:
[Rewriting a Win32 thread context while an exception is in flight](docs/kb/win32-thread-context-rewrite-and-in-flight-exceptions.md).

---

## 롬셋별 설정 파일 / Per-ROM-set configuration files

Task 497부터 PIUIO(JAMMA) 입력의 호스트 키 매핑은 하드코딩이 아니라 `cfg/<롬셋 ID>.ini`에서
읽습니다. 이전에는 같은 매핑이 세 곳에 서로 다른 키 표현으로 중복되어 있었습니다 —
`ScanJammaPort8`와 `CaptureCurrentJammaPressedMask`는 Win32 가상키로,
`glide_opengl_backend.cpp`의 SDL event 경로는 SDL keycode로 정의했습니다. 한 곳만 고치면
조용히 어긋나는 구조였습니다.

정식 키 타입은 `SDL_Keycode`이고 수식키 마스크는 `SDL_Keymod`입니다. SDL event 경로는 변환
없이 직접 비교하고, 폴링 경로만 `SDL_Keycode` → Win32 가상키 변환 하나를 씁니다. 이름 문자열은
SDL 것을 쓰지 않습니다. `SDL_GetScancodeName`이 헤더에서 안정적 양방향 매핑에 부적합하다고
명시하고, 이름에 공백이 들어가며, `not thread safe`이기 때문입니다.

계층은 플랫폼 중립 `repiu::config`(INI 파싱, cfg 탐색, 부모 사슬 레이어링, 첫 실행 생성)와
`repiu::input`(이름 표, 조합키 파싱, 액션 정의와 기본값), 그리고 win32 전용 가상키 변환으로
나뉩니다. 포트·비트·`JammaInputKey`·로그 이름은 `JammaPortBitTable()` 한 표로 합쳐, 로그와
스캔이 어긋날 수 없게 했습니다.

해석은 내장 기본값 → 부모 롬셋 파일 → 롬셋 파일 순으로 **키 단위** 덮어쓰기입니다. 파일이
하나도 없으면 결과는 구현 이전 하드코딩 값과 가상키 단위로 동일하며, probe가 그 표를 직접
전사해 검증합니다. 롬셋 파일이 없으면 해석 결과를 **주석 처리된 템플릿**으로 생성합니다.
활성 항목으로 쓰면 자식 파일이 14개 키를 모두 덮어써 레이어링이 죽기 때문이며, 이 형태 덕분에
"생성이 동작을 바꾸지 않는다"가 구조적으로 성립합니다.

바인딩은 시작 시 1회 해석되어 `SetActiveJammaBindings()`로 설치되고 이후 read-only입니다.
게스트 스레드와 SDL host thread가 동시에 읽지만 불변이므로 락이 없습니다. 스캔 경로에는 문자열
비교·맵 조회·할당이 없고 가상키는 로드 시점에 이미 해석되어 있습니다. 수식키 조회는
`any_binding_uses_modifiers`가 참일 때만, 스냅샷 갱신마다 한 번 수행합니다. 기본 설정은 거짓
이므로 Task 403이 측정한 호스트 키 조회 횟수가 그대로 유지됩니다.

From Task 497 the PIUIO (JAMMA) host key mapping is read from `cfg/<rom-set-id>.ini`
instead of being hardcoded. The same mapping previously lived in three places in three
key vocabularies — `ScanJammaPort8` and `CaptureCurrentJammaPressedMask` in Win32 virtual
keys, and the SDL event path in `glide_opengl_backend.cpp` in SDL keycodes — so changing
one silently desynchronized the others.

`SDL_Keycode` is the canonical key type and `SDL_Keymod` the modifier mask. The SDL event
path compares directly with no translation; only the polling path needs the single
remaining `SDL_Keycode` to Win32 virtual key table. SDL's own name strings are not used:
`SDL_GetScancodeName` documents them as unsuitable for a stable two-way mapping, they
contain spaces, and the lookup is not thread safe.

The layering splits into platform-neutral `repiu::config` (INI parsing, cfg discovery,
parent-chain layering, first-run generation), `repiu::input` (name table, combination
parsing, action definitions and defaults), and win32-only virtual key translation. Port,
bit, `JammaInputKey`, and log name are merged into the single `JammaPortBitTable()`, so
the log and the scan cannot disagree.

Resolution overrides **per key**: built-in defaults, then the parent ROM set's file, then
the ROM set's own. With no file present the result matches the pre-change hardcoded
mapping virtual-key for virtual-key, which the probe verifies against a direct
transcription of that table. When the ROM set has no file, the resolved values are written
as a **commented-out template**; active entries would have the child file override all
fourteen inputs and kill layering, and the commented form makes "generation does not change
behavior" true by construction.

Bindings are resolved once at startup, installed through `SetActiveJammaBindings()`, and
read-only afterwards. The guest thread and the SDL host thread both read them without a
lock because nothing writes. The scan path performs no string comparison, map lookup, or
allocation, and virtual keys are already resolved at load time. Modifier state is read only
when `any_binding_uses_modifiers` is set, once per snapshot refresh. The default
configuration leaves it false, so the host key query count Task 403 measured is unchanged.

사용자 문서는 [docs/guides/romset-config-files.md](docs/guides/romset-config-files.md)입니다.
The user-facing guide is [docs/guides/romset-config-files.md](docs/guides/romset-config-files.md).

---


## 게임패드·조이스틱 입력 / Gamepad and joystick input

issue #34부터 `[Input]` 값에 키 이름과 함께 게임패드·조이스틱 이름을 쓸 수 있습니다.
`Pad<N>_<버튼>`은 SDL이 표준 게임패드로 인식한 장치의 위치 기준 버튼(`A`는 아래쪽 면 버튼)과
트리거, `Joy<N>_Button<K>`·`Joy<N>_Hat<H><방향>`은 표준 매핑이 없는 장치(USB 발판 등)의 번호입니다.
기본값은 표준 게임패드만 두며, Pad1이 1P, Pad2가 2P입니다(위쪽 발판은 어깨 버튼, 아래쪽은 D-pad 아래·왼쪽과 `B`·D-pad 오른쪽, `A`·`Start`가 가운데).

```mermaid
flowchart LR
    subgraph host["SDL host 스레드"]
        E["gamepad/joystick 이벤트"] --> A["SdlPadInput<br/>번호 배정 · HostPadState"]
        A --> M["ComputeJammaPadMask"]
        M -->|바뀐 비트| T["JammaInputTimeline edge"]
        M --> P["PublishJammaPadMask<br/>(atomic)"]
    end
    subgraph guest["게스트 스레드"]
        S["ScanJammaPort8"] --> P
    end
```

* 이름 파서(`repiu/input/host_pad_binding.h`)와 상태(`repiu/input/host_pad_state.h`)는 장치 없이
  검사할 수 있는 순수 데이터이고, `JammaInputBinding`은 키 별칭과 별도로 패드 별칭 4개를 갖습니다.
* 엔진의 `SdlPadInput`은 이벤트를 펌프하는 host 스레드에서 `SDL_INIT_GAMEPAD`를 켜고 장치를 연결
  순서대로 번호 매겨 엽니다. 상태가 바뀌면 바뀐 입력만 타임라인 edge로 기록하고 마스크를 atomic으로
  게시합니다. 폴링 경로는 그 마스크를 atomic load 한 번으로 OR하므로 Task 403의 키 질의 수는 그대로입니다.
* 같은 입력을 키와 패드가 함께 누르면, 한쪽을 떼도 다른 쪽이 누르는 동안 release edge를 남기지 않습니다.
  SDL은 배경 창에 패드 이벤트를 보내지 않으므로, 포커스를 잃으면 키와 같이 패드 상태도 모두 뗍니다.
* 런처도 `SDL_INIT_GAMEPAD`를 켜 ImGui SDL3 백엔드가 게임패드 내비게이션을 쓸 수 있게 합니다.
* issue #52: 한 패드에서 LT+RT+L3+R3을 1초 누르면 종료합니다. 판정(`repiu/input/pad_exit_chord.h`의
  `IsPadExitChordDown`, 1초 연속에 한 번 발동하는 `PadExitChordTimer`)은 `HostPadState`만 보는 순수 코드이고,
  트리거 기준값 `kHostPadTriggerThreshold`도 입력 계층에 있습니다. 게임은 이벤트 펌프마다 `SdlPadInput`의
  상태로 판정해 창 닫기와 같은 `exit_requested_`를 세우고, 런처는 프레임마다 SDL 게임패드를 읽어 같은
  판정으로 창을 닫습니다(`closed_by_exit_chord`). 조합 중 각 버튼은 평소처럼 게임 입력으로도 흐릅니다.
* issue #55: 한 패드의 LT+RT+Y가 OSD를 엽니다(`repiu/input/pad_osd_chord.h`의 `IsPadOsdChordDown`,
  가장자리 감지 `PadChordEdge`). OSD가 열려 있는 동안 `PadGameGate`가 게임에 가는 패드 마스크를 0으로 막고,
  닫힐 때 눌려 있던 비트는 뗄 때까지 숨깁니다. 백엔드는 펌프 끝마다 막힘을 OSD 표시 여부에 맞추고, 그 변화를
  이벤트와 같은 `RecordPadMaskChange`로 edge 기록합니다. OSD는 `NavEnableGamepad`로 패드 탐색을 받습니다.

From issue #34 an `[Input]` value mixes key names with gamepad and joystick names:
`Pad<N>_<Button>` for a device SDL recognizes as a standard gamepad (positional buttons, `A` the
bottom face button, and the triggers), and `Joy<N>_Button<K>`/`Joy<N>_Hat<H><Dir>` by number for
devices without a standard mapping, such as USB dance pads. Defaults cover standard gamepads only,
Pad1 for P1 and Pad2 for P2 (the shoulder buttons for the upper panels, the D-pad's down and left
and `B` with the D-pad's right for the lower ones, `A` and `Start` the center).
The name parser and state are plain data testable without a device, and `JammaInputBinding` keeps
four pad aliases beside its keys. The engine's `SdlPadInput`, on the host thread that pumps events,
initializes `SDL_INIT_GAMEPAD`, numbers devices in connection order, records only the inputs whose
pad state changed as timeline edges, and publishes the mask atomically; the polling path ORs it in
with one atomic load, so Task 403's key query count is unchanged. An input held by a key and a pad
stays held until both release, and losing focus releases pads as it does keys, since SDL sends no
pad events to a background window. The launcher initializes `SDL_INIT_GAMEPAD` too, so ImGui's
SDL3 backend can navigate with a gamepad. Since issue #52 holding LT+RT+L3+R3 on one pad for a second
quits: the check (`IsPadExitChordDown` and the once-per-second-of-holding `PadExitChordTimer` in
`repiu/input/pad_exit_chord.h`) is pure code over `HostPadState`, with the trigger threshold
`kHostPadTriggerThreshold` in the input layer too. The game checks `SdlPadInput`'s state every event pump
and sets the same `exit_requested_` closing the window does; the launcher reads SDL's gamepads every frame
and closes by the same check (`closed_by_exit_chord`). During the chord each button still reaches the game
as usual. Since issue #55 LT+RT+Y on one pad opens the OSD (`IsPadOsdChordDown` and the edge detector
`PadChordEdge` in `repiu/input/pad_osd_chord.h`); while it is open `PadGameGate` zeroes the pad mask the game
sees, and as it closes the bits still held stay hidden until released. The backend keeps suppression in step with
the OSD at the end of every pump and records the change as edges through the same `RecordPadMaskChange` events
use, and the OSD takes pad navigation through `NavEnableGamepad`.

## 롬셋별 NVRAM 저장 / Per-ROM-set NVRAM storage

Task 498부터 93C46 EEPROM 이미지는 MAME와 같이 롬셋별 디렉터리에 저장됩니다.

```
nvram/pumpit1/eeprom.dat
nvram/pumpipx3/eeprom.dat
```

이전에는 모든 롬셋이 작업 디렉터리의 `eeprom.dat` 하나를 공유했습니다. 롬셋 22개가 EEPROM
이미지 하나를 쓰므로 한 타이틀에서 저장한 캐비닛 설정이 다른 타이틀에도 그대로 보였고,
게임마다 EEPROM 레이아웃이 다르므로 실기와도 달랐습니다.

경로 결정은 `repiu::storage::ResolveEepromPath()`가 담당합니다. `REPIU_EEPROM_PATH` 전체
경로 오버라이드가 최우선이며, 벤치마크 스크립트와 측정 가이드가 실행별 EEPROM 격리에 쓰고
있으므로 의미를 유지합니다. 그다음 `nvram` 루트를 `cfg`와 같은 규칙으로 찾습니다 —
`REPIU_NVRAM_DIR` → 작업 디렉터리 → 실행 파일 옆. `rom_set_id`가 빈 프로파일은 롬셋이
아니므로 기존 `eeprom.dat` 경로를 유지합니다.

**디렉터리 생성이 이 함수의 책임입니다.** `Eeprom93c46`는 파일이 없으면 생성자에서 바로
`Save()`로 새 이미지를 쓰는데, `std::ofstream`은 상위 디렉터리가 없으면 조용히 실패합니다.
순서를 어기면 EEPROM이 매 실행 초기화되면서 아무 오류도 남지 않습니다.

롬셋별 파일이 없고 작업 디렉터리에 기존 `eeprom.dat`가 있으면 **한 번 복사해 승계**합니다.
이전까지 그 파일 하나가 모든 롬셋의 상태였으므로, 승계하지 않으면 사용자가 맞춰 둔 캐비닛
설정이 조용히 초기화됩니다. 원본은 남겨 되돌릴 여지를 두고, 대상이 이미 있으면 복사하지
않습니다.

주입은 `SetEepromBackingPath()` 하나입니다. `g_eeprom`은 첫 EEPROM 포트 접근에서 지연
생성되므로 게스트 실행 전에 설정되어야 하며, 이미 열린 뒤의 호출은 무시하고 경고합니다.
소멸자가 이번 실행 내용을 다른 이미지 위에 저장하는 것을 막기 위해서입니다.

From Task 498 the 93C46 EEPROM image is stored per ROM set the way MAME does.

Every ROM set previously shared one `eeprom.dat` in the working directory. With 22 ROM sets
and one image, cabinet settings saved under one title were what another read back, and
since EEPROM layout differs per game this did not match real hardware either.

`repiu::storage::ResolveEepromPath()` owns path resolution. The `REPIU_EEPROM_PATH`
full-path override wins first and keeps its meaning, because benchmark scripts and the
measurement guides use it to isolate the EEPROM per run. The `nvram` root is then located
by the same rule as `cfg`: `REPIU_NVRAM_DIR`, the working directory, then next to the
executable. A profile with an empty `rom_set_id` is not a ROM set and keeps the old
`eeprom.dat` path.

**Creating the directory is that function's responsibility.** `Eeprom93c46` writes a fresh
image through `Save()` from its constructor when the file is missing, and `std::ofstream`
fails silently without a parent directory. Getting the order wrong resets the EEPROM every
run and reports nothing.

When the per-ROM-set file is absent and a legacy `eeprom.dat` exists in the working
directory, it is **copied forward once**. That one file held every ROM set's state until
now, so skipping the copy would silently reset the settings a user had built up. The
original is left in place as a way back, and nothing is copied when the destination already
exists.

Injection is the single `SetEepromBackingPath()`. `g_eeprom` is created lazily on the first
EEPROM port access, so the path must be set before the guest runs; a call after the device
has opened is ignored and reported, which stops the destructor from saving this run's
contents over a different image.

---

## Linux x64 HLE 게스트 쓰기 provenance / Linux x64 HLE guest-write provenance

Linux x64의 bounded guest-write trace는 이제 현재 `GuestCpuContext`를 가진 HLE
`WriteGuestUInt8/16/32` 호출에서 guest EIP와 쓰기 직전 레지스터를 함께 보존한다.
컨텍스트가 없는 host utility 호출은 기존의 zero provenance를 유지한다. 이 계측은
page watch가 활성화된 경우에만 ring에 기록되며, guest memory protection, `memcpy`,
반환값 및 원본 guest code 실행 경로를 변경하지 않는다.

Task 649 당시 `pumpit2a`에서 이 경로는 `0x010F9212 PUSH FS`가 `FS=0`을
당시 반환 slot에 기록했음을 확인했다. Task 652는 이 slot이 allocator의 저장 frame에
속하며, 실제 결함은 앞선 legacy direct CALL이 guest 반환 주소를 만들지 않은 데
있었음을 후속 확인했다. 이 계측 계층 자체는 guest 값을 보정하지 않는다.

The bounded Linux x64 guest-write trace now preserves the guest EIP and
pre-write registers for HLE `WriteGuestUInt8/16/32` calls that have a current
`GuestCpuContext`. Host utility calls without a context retain the previous zero
provenance. The instrumentation records only when the page watch is enabled and
does not change guest memory protection, `memcpy`, return values, or the original
guest-code execution path.

In the Task 649 `pumpit2a` run, this path established that `PUSH FS` at
`0x010F9212` wrote `FS=0` into what was then the consumed return slot. Task 652
subsequently established that the slot belongs to the allocator's saved frame
and that the actual defect was a preceding legacy direct CALL that did not form
its guest return address. This telemetry layer does not repair guest values.

---

## 간접 CALL fallback stack 의미 / Indirect CALL fallback stack semantics

간접 CALL/JMP의 cache miss는 대상 해석 성공 여부와 무관하게 원본 x86 stack 의미를
보존합니다. 공용 `HandleAotIndirectTransfer`는 CALL 대상의 cache 해석 전에 guest 반환
주소 write, ESP 감소, call-frame 기록을 완료합니다. 따라서 해석 실패 후 legacy guest
code로 넘어가도 피호출 함수는 정상적인 반환 slot 위에서 실행됩니다.

선택형 host-dispatch miss tail도 같은 계약을 유지합니다. thunk가 source metadata를
제거한 뒤 CALL fallback은 miss-address slot 하나만 제거하는 `LEA ESP,[ESP+4]`를 사용해
이미 push된 반환 주소를 남기고, JMP fallback은 기존 `LEA ESP,[ESP+8]`로 두 metadata
slot을 모두 제거합니다. 이 정책은 원본 guest code를 수정하지 않으며 DBT/HLE 경계에서
CALL/JMP의 stack 효과만 재현합니다.

An indirect CALL/JMP cache miss preserves the original x86 stack semantics regardless of
target-resolution success. The shared `HandleAotIndirectTransfer` commits a CALL's guest
return-address write, ESP decrement, and call-frame record before resolving the target into
the cache. A failed resolution can therefore enter legacy guest code with a valid return
slot.

The optional host-dispatch miss tail follows the same contract. After its thunk removes
source metadata, CALL fallback removes only the miss-address slot with
`LEA ESP,[ESP+4]`, preserving the already-pushed return address. JMP fallback retains
`LEA ESP,[ESP+8]` and removes both metadata slots. This policy changes no original guest
code; it reproduces only CALL/JMP stack effects at the DBT/HLE boundary.

**Task 737 정정.** 위 규칙은 **x86-64 host에만** 맞습니다. x64의 legacy fallback은 guest의
32-bit CALL을 실행할 수 없어 피호출 함수에서 이어서 실행하므로 push를 미리 해 두어야 합니다.
i386 host의 legacy fallback은 그 CALL을 **원본 그대로 다시 실행**해 반환 주소를 한 번 더 push하므로,
같은 규칙이 반환 주소를 두 번 쌓았고 pumpitea의 호출자 `ret 0x18`이 GL 컨텍스트 포인터로
복귀했습니다. 지금은 해석 실패 시 i386에서만 `HandleAotIndirectTransfer`가 ESP와 call frame을
진입 값으로 되돌리고, i386 전용 host-dispatch miss tail은 CALL fallback도 `LEA ESP,[ESP+8]`로 두
슬롯을 버립니다(probe `indirect_fallback_call_stack_restored`). x86-64 경로는 그대로입니다.

**Task 737 correction.** The rule above is right **for x86-64 hosts only**: their legacy fallback
cannot run a guest 32-bit CALL and continues in the callee, so the push must be made in advance. An
i386 host's legacy fallback **re-executes the CALL natively**, pushing the return address once more,
so the shared rule stacked it twice and pumpitea's caller `ret 0x18` returned into its GL context
pointer. On i386 only, `HandleAotIndirectTransfer` now restores ESP and the call frame to their entry
values when resolution fails, and the i386-only host-dispatch miss tail drops both slots with
`LEA ESP,[ESP+8]` for a CALL as well (probe `indirect_fallback_call_stack_restored`). The x86-64 path
is unchanged.

Linux x64 AOT 재진입에서 planner HLE provenance와 원본 전송 명령 판별이 겹치면
전송 명령을 우선한다. 기존 간접 전송 decoder는 단일 CS override(`2E`)가 붙은
`FF /2`와 `FF /4`를 처리하며, ModRM/SIB effective offset을 현재 source를 포함하는
code selector와 `ResolveSegmentLinearRange`로 해석한다. 이 정책은 LE relocation으로
selector limit을 벗어난 absolute offset에 기존의 검증된 direct-linear fallback을
그대로 적용한 뒤, 읽은 guest target을 공용 AOT target resolver에 전달한다.

When planner-HLE provenance overlaps an original transfer instruction during
Linux x64 AOT reentry, transfer handling takes precedence. The existing
indirect-transfer decoder accepts `FF /2` and `FF /4` with one CS override
(`2E`), resolves the ModRM/SIB effective offset through the code selector that
contains the current source and `ResolveSegmentLinearRange`, and then passes the
loaded guest target to the shared AOT target resolver. This preserves the
existing validated direct-linear fallback for relocated LE absolute offsets
that exceed the selector limit.

---

## Legacy stack run 할당 및 epilogue drain / Legacy stack run allocation and epilogue drain

x64 legacy stack-run helper는 일반/segment PUSH·POP에 이어지는 직접형 `SUB ESP, imm8` 및
`SUB ESP, imm32` 연산을 guest ESP에 직접 적용하고 산술 flag(CF/PF/AF/ZF/SF/OF)를
`SetCompareFlags`로 갱신합니다. 이를 통해 legacy fallback prologue의 지역 변수 공간 할당이
host RSP 대신 guest ESP에서 이루어지도록 보장하여, 후속 epilogue 복원 시 저장 슬롯 레이아웃이
어긋나지 않도록 유지합니다.

또한 segment HLE 처리 후 이어지는 stack 명령 drain을 AOT 상태 flag에 종속되지 않고
연속 처리하도록 허용하며, opcode `0F` directed dispatch에 POP FS/GS를 포함하여 prologue뿐
아니라 epilogue의 segment 및 general POP run도 단일 경계에서 완결되도록 지원합니다.

The x64 legacy stack-run helper applies direct `SUB ESP, imm8` and `SUB ESP, imm32`
operations that follow general or segment PUSH/POP instructions directly to the guest ESP,
updating arithmetic flags (CF/PF/AF/ZF/SF/OF) via `SetCompareFlags`. This ensures that local
stack space allocations in legacy fallback prologues adjust guest ESP rather than host RSP,
preventing slot misalignment during subsequent epilogue restoration.

Furthermore, following segment HLE operations, subsequent stack instructions are drained
in a bounded sequence without dependency on AOT state flags, and opcode `0F` directed
dispatch includes POP FS/GS so that both prologue sequences and epilogue POP runs complete
cleanly at the HLE boundary.

---

## i386 cache와 16-bit 코드 객체 / The i386 cache and 16-bit code objects (Task 737)

i386 emitter의 `kCopy`는 guest 바이트를 그대로 복사하는데, cache는 32-bit 코드 세그먼트에서
실행되므로 **16-bit 코드 객체의 바이트는 다른 명령이 됩니다** — `00 24`는 16-bit에서 `add [si],ah`,
32-bit에서는 잘린 SIB 형식입니다. long-mode emitter는 그런 기록을 이미 INT3 경계로 거절하지만 i386
switch는 그대로 복사했고, pumpitea의 object 3(16-bit 스택 전환 stub, Task 692의 pumpit2a와 같은
코드)이 CFG에 들어가는 dynamic 이미지가 전부 decode 검증에서 거절되었습니다. 실패한 주소는
retire되지 않아 전송마다 재시도되어 guest thread가 시간의 87%를 translation worker 대기에 썼습니다.
Task 737부터 i386 `kCopy`는 `guest_code_default_operand_size == k16`인 기록을 HLE 경계 INT3으로
방출합니다. dynamic 번역 실패 메시지에는 단계(plan/image)와 첫 decode 실패 항목(guest 주소,
바이트·명령 수, 방출 바이트)이 들어갑니다. 거절된 이미지는 실행되지 않으므로 여기가 유일한 진단
지점입니다.

The i386 emitter's `kCopy` copies guest bytes verbatim, and the cache runs under a 32-bit code
segment, so **a 16-bit code object's bytes become different instructions** -- `00 24` is
`add [si],ah` in 16-bit code and a truncated SIB form in 32-bit. The long-mode emitter already
refuses such records with an INT3 boundary, but the i386 switch copied them, and every dynamic image
whose CFG reached pumpitea's object 3 (the 16-bit stack-switch stub, the same code Task 692 found in
pumpit2a) was rejected by the decode check. Failed addresses are not retired and were retried on every
transfer, leaving the guest thread waiting for the translation worker 87% of the time. Since Task 737
the i386 `kCopy` emits a record with `guest_code_default_operand_size == k16` as an HLE boundary
INT3. The dynamic translation failure message now carries the stage (plan/image) and the first decode
failure sample (guest address, byte and instruction counts, emitted bytes); a rejected image never
runs, so this is the only place to diagnose it.

## Linux x64 혼합 모드 AOT 및 16-bit 스택 레지스터 lowering

Task 693에서 공용 guest HLE dispatcher의 EA 분기는 기존 LINEXE service
boundary를 먼저 확인합니다. AOT에서 guest EIP가 복원된 경우에도 fault-level
경로와 같은 loader 서비스가 적용됩니다. 미처리 전이는 일반 far-jump로
넘기며, 읽을 수 없는 bridge frame은 이전 관측 frame으로 대체하지 않습니다.

In Task 693, shared guest HLE EA dispatch checks the existing LINEXE service
boundary first. AOT-restored guest EIPs receive the same loader service handling
as the fault-level path. Unhandled transfers continue to generic far-jump
handling; unreadable bridge frames never reuse an earlier observed frame.

Task 701부터 LINEXE `GETPROCADDR`가 만드는 guest-visible far pointer의 selector는
물리 `GuestCpuContext::SegCs`가 아니라 wrapper continuation을 포함하는 유일한
실행 가능 guest descriptor에서 구합니다. Linux x64의 물리 CS는 host selector
`0x33`이므로 guest ABI에 노출할 수 없습니다. descriptor가 없거나 모호하거나
실행 불가이면 결과 버퍼와 성공 반환 상태를 확정하지 않고 fail closed합니다.

Starting with Task 701, the selector in a guest-visible far pointer produced by
LINEXE `GETPROCADDR` comes from the unique executable guest descriptor that
contains the wrapper continuation, rather than physical
`GuestCpuContext::SegCs`. Linux x64 physical CS is host selector `0x33` and must
not cross the guest ABI. A missing, ambiguous, or non-executable descriptor
fails closed before committing the result buffer or successful return state.

Task 702부터 fault-level Glide gate가 guest continuation으로 반환하면 Linux x64는
이를 handled guest boundary로 분류하고 공용 post-HLE AOT 재진입을 수행합니다.
따라서 `PUSH`, operand width, address-size처럼 long mode와 의미가 다른 다음 guest
명령은 원본 bytes로 실행되지 않고 검증된 cache lowering을 사용합니다. 재진입에
실패한 non-identical continuation은 기존 정책대로 fail closed하며, i386과 AOT
미사용 경로는 변경하지 않습니다.

Starting with Task 702, when a fault-level Glide gate returns to a guest
continuation, Linux x64 classifies it as a handled guest boundary and performs
the shared post-HLE AOT re-entry. The following guest instruction therefore
uses verified cache lowering instead of executing original bytes when `PUSH`,
operand width, address size, or other semantics differ in long mode. A failed
non-identical continuation remains fail-closed; i386 and non-AOT paths are
unchanged.

Task 703부터 실행할 이전 INT 8 handler가 없는 타이머 체인 경계도 EFLAGS stack
정리를 마친 guest continuation을 handled guest boundary로 재진입시킵니다. 따라서
경계 직후의 `CALL`, `PUSHFD` 같은 stack 명령은 원본 long-mode bytes가 아니라 R15D
guest stack lowering을 사용합니다. 타이머 체인의 selector 판정과 EFLAGS 폐기 규칙은
그대로이며, 재진입할 수 없는 non-identical continuation은 fail closed합니다.

Starting with Task 703, the timer-chain boundary for an absent predecessor INT 8
handler also re-enters its post-cleanup guest continuation as a handled guest
boundary. Stack instructions such as `CALL` and `PUSHFD` immediately after the
boundary therefore use R15D guest-stack lowering rather than original long-mode
bytes. Timer-chain selector classification and EFLAGS cleanup are unchanged,
and a non-identical continuation that cannot re-enter still fails closed.

Task 704부터 Linux x64 INT 8 주입 frame은 signal context의 물리 host CS가 아니라
중단된 EIP를 포함하는 유일한 실행 가능 guest descriptor의 logical selector를
기록합니다. lookup 실패는 frame을 쓰거나 pending tick을 소비하기 전에 주입을
보류합니다. 원본 ISR 끝의 prefix 없는 32-bit `IRETD`는 전용 HLE가 12바이트
EIP/CS/EFLAGS frame과 executable k32 복귀 target을 검증한 뒤 EIP, CS, EFLAGS,
ESP를 원자적으로 복원합니다. 성공한 fault-level 복귀는 handled-boundary AOT
재진입을 사용하며, 잘못된 frame은 guest context를 변경하지 않고 fail closed합니다.
16-bit IRET, privilege 전환, i386 native 경로는 이 정책의 대상이 아닙니다.

Starting with Task 704, Linux x64 INT 8 injection records the logical selector
of the unique executable guest descriptor containing the interrupted EIP,
rather than the physical host CS from the signal context. A failed lookup
defers delivery before writing a frame or consuming the pending tick. Dedicated
HLE for the original ISR's unprefixed 32-bit `IRETD` validates the 12-byte
EIP/CS/EFLAGS frame and executable k32 return target, then atomically restores
EIP, CS, EFLAGS, and ESP. A successful fault-level return uses handled-boundary
AOT re-entry, while an invalid frame fails closed without changing guest state.
16-bit IRET, privilege transitions, and the native i386 path are outside this
policy.

Task 692는 확인된 mode16 register PUSH의 HLE 처리를 `mode16_stack_push`로
분리합니다. 공용 `guest_stack_access`가 SS.B에 따른 SP/ESP 갱신, SS.base 주소
변환, descriptor 권한/limit 검사를 수행합니다. 기존 R15D에는 guest ESP 값을
유지하며 mode16에서 이 값을 곧바로 선형 메모리 주소로 사용하지 않습니다.
일반 POP 및 expand-down stack 지원은 이 PUSH 구현에 포함되지 않습니다.

Task 692 separates confirmed mode16 register PUSH handling into mode16_stack_push.
Shared guest_stack_access computes SP/ESP updates from SS.B, translates through
SS.base and validates descriptor permissions/limits. R15D retains guest ESP;
the mode16 PUSH adapter does not treat it directly as a linear memory address.
General POP and expand-down stacks are outside this PUSH implementation.

Task 694는 별도의 mode16 bare RETF handler를 추가합니다. `GuestStackReadAccess`는
동일한 present/writable 정책, SS.B width 규칙, SS.base 변환, descriptor limit
검사를 사용해 현재 SS:SP read window를 계산합니다. EIP를 포함하는 유효한
guest `SegCs`를 현재 code 식별에 우선 사용하고, guest CS를 제공하지 않는
AOT 문맥에서는 기존의 유일한 EIP 역조회를 사용합니다. handler는 executable
mode16 code의 prefix 없는 `CB`만 받아 SS를 통해 4바이트 word IP/CS frame을
읽고, executable selector-relative descriptor를 통해서만 target을 해석합니다.
공용 guest dispatcher와 fault HLE chain 모두 이 adapter를 사용하며, 기존
mode16 `66 CB` 8바이트 resolver와 generic 32-bit return 경로는 변경하지
않습니다. 잘못된 selector, stack geometry, unreadable frame, non-mode16 code는
context를 변경하지 않습니다.

Task 694 adds a separate mode16 bare RETF handler. `GuestStackReadAccess`
computes the current SS:SP read window with the same present/writable policy,
SS.B width rules, SS.base translation, and descriptor-limit checks. A valid
guest `SegCs` covering EIP is preferred for current-code identification; AOT
contexts that omit guest CS use the existing unique EIP reverse lookup. The
handler accepts only prefix-free `CB` in executable mode16 code, reads a
four-byte word IP/CS frame through SS, and resolves the target only through an
executable selector-relative descriptor. The shared guest dispatcher and fault
HLE chain both use this adapter, while the existing mode16 `66 CB` eight-byte
resolver and generic 32-bit return path remain unchanged. Invalid selectors,
stack geometry, unreadable frames, and non-mode16 code leave the context
unchanged.

LE 실행 파일 object flag가 공용 AOT planner의 `LEGACY_16` 또는
`LEGACY_32`를 선택하며, 선택된 `GuestCodeDefaultOperandSize`는 모든
`AotInstructionRecord`에 담겨 cache emission과 dynamic append까지 전달됩니다.
따라서 16-bit object가 다음 명령의 바이트를 잘못된 32-bit immediate로
소비하지 않습니다.

현재 증명된 첫 16-bit long-mode lowering은 `MOV SP, imm16` (`BC iw`)입니다.
x64에서 guest ESP는 R15D에 보관되므로 cache는 `66 41 BF iw`를 방출하여
R15W만 갱신하고 host RSP는 건드리지 않습니다. 전용 증명이 없는 16-bit
record는 32-bit native slot에 들어가지 않고 INT3 boundary가 됩니다. 일반
16-bit POP, segment, expand-down stack ABI는 별도 공용 설계 단위에서
다룹니다.

```mermaid
flowchart LR
    FLAGS[LE object flags] --> MODE[Guest code mode]
    MODE --> RECORD[AotInstructionRecord]
    RECORD -->|16-bit BC iw| R15W[66 41 BF iw -> R15W]
    RECORD -->|unproven 16-bit| INT3[INT3 HLE boundary]
    RECORD -->|32-bit| X64[Existing x64 lowering]
```

---

## Linux x64 mixed-mode AOT and 16-bit stack-register lowering

LE executable object flags now select `LEGACY_16` or `LEGACY_32` for the shared
AOT planner, and the selected `GuestCodeDefaultOperandSize` travels with every
`AotInstructionRecord` into cache emission and dynamic append. This prevents a
16-bit object from consuming the following bytes as a false 32-bit immediate.

The first proven 16-bit long-mode lowering is `MOV SP, imm16` (`BC iw`). Since
guest ESP is held in R15D on x64, the cache emits `66 41 BF iw`, which updates
R15W and leaves host RSP untouched. 16-bit records without a dedicated proof,
including non-copy control-flow records, are emitted as INT3 boundaries rather
than entering 32-bit native slots. General 16-bit POP, segment, and
expand-down stack ABI remain separate shared design work.

```mermaid
flowchart LR
    FLAGS[LE object flags] --> MODE[Guest code mode]
    MODE --> RECORD[AotInstructionRecord]
    RECORD -->|16-bit BC iw| R15W[66 41 BF iw -> R15W]
    RECORD -->|unproven 16-bit| INT3[INT3 HLE boundary]
    RECORD -->|32-bit| X64[Existing x64 lowering]
```

---

## Linux x64 dynamic AOT emitted-slot trace / Linux x64 dynamic AOT emitted-slot trace

Linux x64 dynamic AOT translation에는 거절된 `contains` 주소를 진단하기 위한 선택형
image trace가 있습니다. `REPIU_AOT_FALLBACK_TRACE`와 일치하는
`REPIU_AOT_DYNAMIC_CONTAINS`를 함께 설정하면 `src/engine/aot_code_cache.cpp`가 해당
address-map entry, bounded emitted bytes, 그리고 존재할 경우 대응하는
`AotSegmentOverrideSite` offset을 출력합니다. 이 trace는 coverage validator가 검사한
실제 cache slot과 guard layout을 식별하지만 emission, validation, guest semantics,
fallback 선택, 기본 quiet path는 변경하지 않습니다.

이 진단은 image 생성 후 dynamic append 지점에 연결되어 한 번의 실행에서 plan record와
emitted representation을 비교할 수 있게 합니다. 원본 guest code를 실행의 기준으로
유지하면서 다음 호환성 결정을 위해 필요한 HLE 경계 상태만 노출합니다.

Linux x64 dynamic AOT translation has an opt-in image trace for diagnosing a rejected
`contains` address. When both `REPIU_AOT_FALLBACK_TRACE` and a matching
`REPIU_AOT_DYNAMIC_CONTAINS` are set, `src/engine/aot_code_cache.cpp` prints the matching
address-map entry, a bounded copy of its emitted bytes, and the corresponding
`AotSegmentOverrideSite` offsets when one exists. The trace identifies the actual cache slot
and guard layout that coverage validation examined; it does not alter emission, validation,
guest semantics, fallback selection, or the default quiet path.

The diagnostic is intentionally attached to dynamic append after image construction, so a
plan record and its emitted representation can be compared in one run. This keeps the
original guest code as the execution source while exposing only HLE boundary state needed
for the next compatibility decision.

---

## Linux x64 segment coverage predicate parity / Linux x64 segment coverage predicate parity

Linux x64 segment-override coverage validation은 해당 slot을 생성하는 emitter와
동일한 주소 형식 및 suffix layout 규칙을 사용해야 합니다. `mod=00 && rm=5`만
absolute disp32 형식이며, `rm=7`인 `EDI` base 형식은 base register를 보존한
disp32 형식입니다. 원본 displacement가 없는 형식의 suffix 시작점은
`ModRM + 1`이고, displacement가 있는 형식은 displacement 끝입니다.

이 predicate parity는 `ValidateAotCodeCacheHleCoverage`가 실제 emitted access
bytes를 잘못된 absolute/SIB 형식으로 해석하거나 suffix를 중복 복사하는 것을
막습니다. 변경은 validator와 regression probe에 한정되며, 원본 guest 명령,
segment selector guard, memory semantics, 기본 i386 emission은 유지됩니다.

On Linux x64, segment-override coverage validation must use the same address-form and
suffix-layout rules as the emitter that creates the slot. Only `mod=00 && rm=5` is the
absolute disp32 form; `rm=7` preserves `EDI` as the base register in the widened disp32
form. When the original form has no displacement, the suffix begins at `ModRM + 1`; when
it has a displacement, the suffix begins after that displacement.

This predicate parity prevents `ValidateAotCodeCacheHleCoverage` from interpreting the
actual emitted access bytes as the wrong absolute/SIB form or from copying a suffix twice.
The change is limited to the validator and its regression probe; the original guest
instruction, segment selector guard, memory semantics, and default i386 emission remain
unchanged.

---

## Legacy fallback direct CALL HLE / Legacy fallback direct CALL HLE

Linux x64에서 AOT coverage가 거절된 original span은 Trap Flag 아래 실행됩니다. 이때
`E8 rel32`를 CPU에 직접 실행시키면 long mode CALL이 host call stack만 변경하고 별도로
유지되는 guest ESP에는 32-bit 반환 주소를 만들지 않습니다. shared HLE dispatcher는
`aot_legacy_fallback`에서 이 형식을 선점하여 target을 계산하고 guest `[ESP-4]`에
fallthrough를 기록한 뒤 guest ESP와 EIP를 갱신합니다.

이 handler는 기존 AOT call-frame 및 call/return trace도 기록합니다. 처리 후에는 공용
post-HLE reentry가 target cache를 선택하거나 guest target에서 TF bridge를 계속합니다.
i386 직접 실행과 AOT cache가 이미 lowering한 CALL은 이 경로를 사용하지 않습니다.

On Linux x64, an original span rejected by AOT coverage executes under Trap Flag.
Executing `E8 rel32` directly there would let long-mode CALL modify only the host
call stack, without creating a 32-bit return address in the separately maintained
guest ESP. The shared HLE dispatcher intercepts this form during
`aot_legacy_fallback`, calculates its target, writes the fallthrough to guest
`[ESP-4]`, and updates guest ESP and EIP.

The handler also records the existing AOT call frame and call/return trace. The
common post-HLE reentry then selects a target cache entry or keeps the TF bridge
at the guest target. Native i386 execution and CALLs already lowered in the AOT
cache do not use this path.

---

## Linux x64 미해석 반환 legacy bridge / Linux x64 unresolved-return legacy bridge

Linux x64 return resolver가 guest 반환 target을 cache로 해석하지 못해도 곧바로 모든
경우를 종료하지는 않습니다. target이 guest arena 안에 있고 공용 long-mode classifier가
첫 명령을 `kIdenticalBytes`로 증명한 경우에만, resolver는 target을 dispatch frame의
`guest_continuation`에 기록하고 전용 legacy-resume thunk를 선택합니다. 이 제한은 stack,
segment, 상대 제어 전송처럼 long mode에서 의미가 달라지는 명령을 원본 주소에서 실행하지
못하게 합니다.

legacy-resume thunk는 guest EAX를 먼저 복원하고, resolver 호출 전에 저장된 guest
EFLAGS에 TF를 설정한 뒤 연속된 `POPFQ; JMP guest_continuation`으로 끝납니다. jump가
새로 설정된 TF를 소비하는 host 명령이 되므로 #DB는 guest target 경계를 보고하고 기존
single-step/HLE 경로가 뒤의 민감한 guest 명령 실행 전에 제어를 넘겨받습니다. target이
0이거나 arena 밖이거나 첫 명령의 동일성이 증명되지 않으면 기존 zero 반환과 INT3
fail-closed 동작을 유지합니다.

When the Linux x64 return resolver cannot map a guest return target into the
cache, it does not relax every failure. Only a target inside the guest arena
whose first instruction the shared long-mode classifier proves to be
`kIdenticalBytes` is stored in the dispatch frame's `guest_continuation` and
directed to a dedicated legacy-resume thunk. This excludes stack, segment, and
relative-control instructions whose long-mode behavior differs.

The legacy-resume thunk restores guest EAX before activating TF, then ends with
the consecutive `POPFQ; JMP guest_continuation` sequence. The jump is the host
instruction that consumes the newly activated TF, so #DB reports the guest
target boundary and the existing single-step/HLE path takes over before later
guest-sensitive instructions execute. A zero target, a target outside the arena,
or an unproven first instruction keeps the existing zero result and INT3
fail-closed behavior.

Linux x64의 `REPIU_LINUX_X64_GUEST_ENTRY_TRACE=<start>` 진단은 선택적
`REPIU_LINUX_X64_GUEST_ENTRY_TRACE_END=<end>`와 함께 inclusive guest 주소 범위를
선택할 수 있습니다. cache 주소는 guest map으로 역변환한 뒤 범위와 비교하며, 끝 주소가
없거나 시작보다 작으면 기존 exact 선택으로 동작합니다.

On Linux x64, the `REPIU_LINUX_X64_GUEST_ENTRY_TRACE=<start>` diagnostic can
select an inclusive guest-address range with the optional
`REPIU_LINUX_X64_GUEST_ENTRY_TRACE_END=<end>`. Cache addresses are reverse
mapped before matching; an absent or smaller end retains the original exact
selection.

---

## Legacy fallback moffs32 store HLE

동적 AOT 계획이 뒤쪽 경계 때문에 전체 거절된 원본 span에서는, AOT cache가 이미
지원하는 일부 재인코딩 대상이 다시 legacy single-step 경로에 나타날 수 있습니다.
prefix 없는 `A3 disp32`는 32-bit guest에서 5바이트 `MOV [moffs32],EAX`이지만 long
mode에서는 8바이트 주소를 소비하므로 원본 bytes를 직접 실행할 수 없습니다. 공용
memory-store HLE는 이 형식을 명시적으로 decode하여 기존 guest writable 검사와
`WriteGuestUInt32`를 통해 32-bit 주소에 기록하고 EIP를 5 증가시킵니다. MOV의 EFLAGS는
보존되며, arena 밖 destination은 기존처럼 거부됩니다.

When a dynamic AOT plan is rejected as a whole by a later boundary, original
legacy single-step can encounter an instruction whose re-encoding is already
supported in the cache. Unprefixed `A3 disp32` is a five-byte
`MOV [moffs32],EAX` in the 32-bit guest, but long mode consumes an eight-byte
address, so its original bytes cannot execute directly. Shared memory-store HLE
decodes this form explicitly, writes to the 32-bit address through the existing
guest-writable check and `WriteGuestUInt32`, and advances EIP by five. MOV flags
are preserved, and an out-of-arena destination remains refused.

---

## 간접 CALL fallback stack 의미 / Indirect CALL fallback stack semantics

간접 CALL/JMP의 cache miss는 대상 해석 성공 여부와 무관하게 원본 x86 stack 의미를
보존합니다. 공용 `HandleAotIndirectTransfer`는 CALL 대상의 cache 해석 전에 guest 반환
주소 write, ESP 감소, call-frame 기록을 완료합니다. 따라서 해석 실패 후 legacy guest
code로 넘어가도 피호출 함수는 정상적인 반환 slot 위에서 실행됩니다.

선택형 host-dispatch miss tail도 같은 계약을 유지합니다. thunk가 source metadata를
제거한 뒤 CALL fallback은 miss-address slot 하나만 제거하는 `LEA ESP,[ESP+4]`를 사용해
이미 push된 반환 주소를 남기고, JMP fallback은 기존 `LEA ESP,[ESP+8]`로 두 metadata
slot을 모두 제거합니다. 이 정책은 원본 guest code를 수정하지 않으며 DBT/HLE 경계에서
CALL/JMP의 stack 효과만 재현합니다.

An indirect CALL/JMP cache miss preserves the original x86 stack semantics regardless of
target-resolution success. The shared `HandleAotIndirectTransfer` commits a CALL's guest
return-address write, ESP decrement, and call-frame record before resolving the target into
the cache. A failed resolution can therefore enter legacy guest code with a valid return
slot.

The optional host-dispatch miss tail follows the same contract. After its thunk removes
source metadata, CALL fallback removes only the miss-address slot with
`LEA ESP,[ESP+4]`, preserving the already-pushed return address. JMP fallback retains
`LEA ESP,[ESP+8]` and removes both metadata slots. This policy changes no original guest
code; it reproduces only CALL/JMP stack effects at the DBT/HLE boundary.

**Task 737 정정.** 위 규칙은 **x86-64 host에만** 맞습니다. x64의 legacy fallback은 guest의
32-bit CALL을 실행할 수 없어 피호출 함수에서 이어서 실행하므로 push를 미리 해 두어야 합니다.
i386 host의 legacy fallback은 그 CALL을 **원본 그대로 다시 실행**해 반환 주소를 한 번 더 push하므로,
같은 규칙이 반환 주소를 두 번 쌓았고 pumpitea의 호출자 `ret 0x18`이 GL 컨텍스트 포인터로
복귀했습니다. 지금은 해석 실패 시 i386에서만 `HandleAotIndirectTransfer`가 ESP와 call frame을
진입 값으로 되돌리고, i386 전용 host-dispatch miss tail은 CALL fallback도 `LEA ESP,[ESP+8]`로 두
슬롯을 버립니다(probe `indirect_fallback_call_stack_restored`). x86-64 경로는 그대로입니다.

**Task 737 correction.** The rule above is right **for x86-64 hosts only**: their legacy fallback
cannot run a guest 32-bit CALL and continues in the callee, so the push must be made in advance. An
i386 host's legacy fallback **re-executes the CALL natively**, pushing the return address once more,
so the shared rule stacked it twice and pumpitea's caller `ret 0x18` returned into its GL context
pointer. On i386 only, `HandleAotIndirectTransfer` now restores ESP and the call frame to their entry
values when resolution fails, and the i386-only host-dispatch miss tail drops both slots with
`LEA ESP,[ESP+8]` for a CALL as well (probe `indirect_fallback_call_stack_restored`). The x86-64 path
is unchanged.

---

## Legacy stack run 할당 및 epilogue drain / Legacy stack run allocation and epilogue drain

x64 legacy stack-run helper는 일반/segment PUSH·POP에 이어지는 직접형 `SUB ESP, imm8` 및
`SUB ESP, imm32` 연산을 guest ESP에 직접 적용하고 산술 flag(CF/PF/AF/ZF/SF/OF)를
`SetCompareFlags`로 갱신합니다. 이를 통해 legacy fallback prologue의 지역 변수 공간 할당이
host RSP 대신 guest ESP에서 이루어지도록 보장하여, 후속 epilogue 복원 시 저장 슬롯 레이아웃이
어긋나지 않도록 유지합니다.

또한 segment HLE 처리 후 이어지는 stack 명령 drain을 AOT 상태 flag에 종속되지 않고
연속 처리하도록 허용하며, opcode `0F` directed dispatch에 POP FS/GS를 포함하여 prologue뿐
아니라 epilogue의 segment 및 general POP run도 단일 경계에서 완결되도록 지원합니다.

The x64 legacy stack-run helper applies direct `SUB ESP, imm8` and `SUB ESP, imm32`
operations that follow general or segment PUSH/POP instructions directly to the guest ESP,
updating arithmetic flags (CF/PF/AF/ZF/SF/OF) via `SetCompareFlags`. This ensures that local
stack space allocations in legacy fallback prologues adjust guest ESP rather than host RSP,
preventing slot misalignment during subsequent epilogue restoration.

Furthermore, following segment HLE operations, subsequent stack instructions are drained
in a bounded sequence without dependency on AOT state flags, and opcode `0F` directed
dispatch includes POP FS/GS so that both prologue sequences and epilogue POP runs complete
cleanly at the HLE boundary.

---

# HLE 재진입 메모와 breakpoint 지점 census (Task 741)

AOT cache 안의 HLE boundary(INT3)에서 돌아오는 재진입은 재개 지점의 guest 명령을 세 방향으로
판단합니다: segment register를 쓰는가(`ProbeGuestInstructionSegmentWrite`), 다음 control transfer까지
HLE boundary 없이 이어지는가(`IsImmediateHleReentrySpanSafe`, 최대 64 명령), long mode에서 바이트가
동일한가(`CanResumeLinuxX64LegacyTarget`). 셋 다 Zydis 디코드이고 답은 guest 바이트, code segment의
default operand size, HLE boundary 목록에만 의존하므로 `ThreadContext::AotReentryMemoEntry`(1,024칸
direct-mapped)에 주소별로 기억합니다. 항목은 guest 코드 쓰기가 완료될 때마다 오르는 세대와 주소의
첫 8바이트 지문이 맞는 동안만 유효하고, long mode 답은 계산 당시의 code mode도 함께 보관합니다.
`REPIU_AOT_REENTRY_MEMO=0`이 메모를 끕니다. 같은 작업이 넣은 breakpoint 지점 census는 VEH 초입에서
host EIP별로 breakpoint 예외를 세고(512칸 open addressing) exit site를 누적하며, 최종 보고가 상위
32개를 `host/guest/cache/count/exits`로 찍습니다. 어느 guest 지점이 trap을 내는지 묻는 첫 도구입니다.

# HLE reentry memo and breakpoint site census (Task 741)

A reentry from an HLE boundary (INT3) inside the AOT cache judges the guest instruction it resumes at
three ways: does it write a segment register (`ProbeGuestInstructionSegmentWrite`), does it reach the
next control transfer without an HLE boundary (`IsImmediateHleReentrySpanSafe`, up to 64
instructions), and are its bytes identical in long mode (`CanResumeLinuxX64LegacyTarget`). All three
are Zydis decodes whose answers depend only on the guest bytes, the code segment's default operand
size and the HLE boundary list, so they are remembered per address in
`ThreadContext::AotReentryMemoEntry` (1,024 direct-mapped slots). An entry stays valid while the
generation, which moves on every completed guest code write, and the eight-byte fingerprint of the
address still match; the long-mode answer also keeps the code mode it was computed under.
`REPIU_AOT_REENTRY_MEMO=0` turns the memo off. The breakpoint site census from the same task counts
breakpoint exceptions by host EIP at the VEH entry (512-slot open addressing), accumulates the exit
site, and the final report prints the 32 busiest as `host/guest/cache/count/exits`: the first
instrument that answers which guest site is trapping.

# long mode의 segment 읽기·push 슬롯 (Task 742)

Linux x64 AOT cache에서 `mov r16/r32, sreg`(`kGuardedSegmentRead`)와 32비트 `push es/ds/fs/gs`
(`kGuardedSegmentPush`)는 guard 없는 슬롯으로 방출됩니다. 두 명령의 값은 guest 눈에 보이는 selector이고
엔진이 `shadow_selectors->selectors[seg]`에 유지하므로(guest의 segment 쓰기마다 갱신, guarded load는
shadow와 같은 값만 통과), 슬롯은 그 shadow를 읽습니다: read는 `movzx r32, word [shadow]`(16비트 형식은
`mov r16, word [shadow]`), push는 `lea r15d,[r15-4]; movzx r14d, word [shadow]; mov [r15], r14d`. site는
`AotGuardedSegmentReadSite`를 재사용하며 `PatchAotGuardedSegmentReadSites`가 shadow 주소를 채우고,
shadow가 없는 segment의 슬롯은 INT3로 닫힙니다. i386 이미지는 read를 host segment register 비교
슬롯으로, push를 원본 바이트 복사로 방출하며, long-mode 이미지에는 i386 read 슬롯을 방출하지 않습니다.

# Long-mode segment read and push slots (Task 742)

In the Linux x64 AOT cache, `mov r16/r32, sreg` (`kGuardedSegmentRead`) and a 32-bit `push
es/ds/fs/gs` (`kGuardedSegmentPush`) are emitted as guard-free slots. Both produce the guest-visible
selector, which the engine keeps in `shadow_selectors->selectors[seg]` (updated on every guest segment
write; the guarded load admits only a value equal to the shadow), so the slots read that shadow: the
read as `movzx r32, word [shadow]` (`mov r16, word [shadow]` for the 16-bit form), the push as `lea
r15d,[r15-4]; movzx r14d, word [shadow]; mov [r15], r14d`. The site reuses `AotGuardedSegmentReadSite`,
`PatchAotGuardedSegmentReadSites` fills in the shadow address, and a slot for a segment without a
shadow closes with INT3. i386 images emit the read as the host-segment-register compare slot and copy
the push verbatim; the i386 read slot is never emitted into a long-mode image.

# Linux x64에서 번역되지 않은 대상으로의 return (Task 743)

`RepiuLinuxX64ReturnThunk`가 부르는 `LinuxX64EngineResolver`는 return 대상이 cache에 없고 동적
번역도 실패하면 `RepiuLinuxX64LegacyResumeThunk`를 돌려줍니다(대상이 읽을 수 있는 guest 코드인 한).
그 thunk는 guest 레지스터를 복원하고 TF를 켠 채 `jmp`하므로 #DB가 대상에서 아무것도 실행하기 전에
나고, VEH의 단일 스텝 경로가 간접 call의 legacy fallback과 같이 HLE·stack bridge로 이어갑니다. 첫
명령이 long mode에서 byte-identical일 것을 요구하던 이전 규칙은 그 밖의 대상에서 thunk의 `int3`로
죽게 했습니다. 번역도 없고 동일하지도 않은 명령에 이르면 여전히 실행할 수 없고, 그때는
`[repiu-x64-untranslatable] stage= eip= bytes= …`를 남기고 `[repiu-fault] unhandled …`로 끝납니다.
그 보고에는 `NoteVehExitSite`가 platform 전역에 쓴 마지막 VEH exit site(`last_exit_site=`)가 붙습니다.
`REPIU_AOT_DYNAMIC_REJECT=<guest 주소>|read`는 worker가 그 동적 이미지를 거절하게 해 이 경로를
재현합니다.

# Returning to an untranslated target on Linux x64 (Task 743)

`LinuxX64EngineResolver`, called from `RepiuLinuxX64ReturnThunk`, returns
`RepiuLinuxX64LegacyResumeThunk` when the return target is not in the cache and dynamic translation
fails, as long as the target is readable guest code. That thunk restores the guest registers and jumps
with TF set, so the #DB lands at the target before anything there runs, and the VEH's single-step path
continues through the HLE and stack-bridge handlers as the indirect call's legacy fallback does. The
earlier rule, a byte-identical first instruction in long mode, sent every other target to the thunk's
`int3`. An instruction with no translation and no long-mode identity still cannot run; the run then
ends in `[repiu-fault] unhandled …` after `[repiu-x64-untranslatable] stage= eip= bytes= …`, and the
report carries the last VEH exit site (`last_exit_site=`) that `NoteVehExitSite` publishes to a
platform global. `REPIU_AOT_DYNAMIC_REJECT=<guest address>|read` makes the worker reject those dynamic
images to reproduce the path.

# swap interval 기본값 (Task 766)

GL 창이 만들어지면 엔진은 항상 `SDL_GL_SetSwapInterval`을 한 번 부릅니다. 값은
`ReadGlideSwapInterval`(`glide_swap_interval_policy`)이 정합니다. `REPIU_GLIDE_SWAP_INTERVAL`이
유효하면(`-1`~`4`) 그 값이고, 없거나 형식이 틀리면 `kDefaultGlideSwapInterval`(1, vsync 켜짐)입니다.
런처의 `[Video] swap_interval`은 같은 환경 변수로 게시됩니다. 드라이버나 SDL에 기본값을 맡기지 않는 이유는
그 기본값이 호스트마다 달라서입니다. SDL의 Wayland backend는 요청이 없으면 상한 없이 그립니다. 드라이버가
요청을 거부하면 아래 절의 페이싱이 대신합니다. 최종 보고의 `override requested/value/applied/effective`가
값의 출처와 결과를 보여 줍니다.

# The swap interval default (Task 766)

Once a GL window exists the engine always calls `SDL_GL_SetSwapInterval` once, with the value
`ReadGlideSwapInterval` (`glide_swap_interval_policy`) resolves: a valid `REPIU_GLIDE_SWAP_INTERVAL`
(`-1` to `4`), or `kDefaultGlideSwapInterval` (1, vsync on) when it is absent or malformed. The
launcher's `[Video] swap_interval` is published into the same variable. The default is not left to
the driver or SDL because it differs by host: SDL's Wayland backend draws uncapped unless asked. A
refused request falls to the pacing described in the next section, and the final report's
`override requested/value/applied/effective` line shows where the value came from and what took.

# swap 대기와 타이머 tick, MP3 재생 시계 (Tasks 745–750)

`grBufferSwap` 게이트는 Linux x64에서 present를 호스트 스레드에 게시하고(`PostBufferSwap`) 게스트 스레드가
`WaitForPendingSwaps`로 기다립니다. 호스트 스레드의 present는 드라이버의 vsync이거나, 드라이버가 swap
interval을 거부했을 때의 페이싱(`PaceSwapAfterPresent`: 주사율 기준 마감 누적, 한 주기 이상 밀리면 재동기)
입니다. 기다리는 동안 tick이 밀리면 `ContinueGlideSwapWait`가 반환 주소를 pop한 ESP에 인터럽트 프레임을
쌓고 EIP를 게이트에 도달한 `call`의 주소로 둔 채 `InjectPendingInterrupts`를 부릅니다. ISR의 `iret`는
`call`을 다시 실행하고, 게이트는 `ThreadContext::glide_swap_wait_active`를 보고 계속 기다리며, present가
끝났을 때만 호출자에게 반환합니다. 반환 목표가 게이트가 아니라 호출부인 이유는 게이트의 세그먼트(LINEXE
0x0080) 디스크립터가 실행 가능으로 표시돼 있지 않아 프레임의 CS 조회가 거절하기 때문입니다. 호출부는
`FindGlideGateCallSite`가 `call rel32` → 게이트 또는 import thunk(`jmp rel32`, `jmp dword ptr [m32]`)로
검증하며, 검증되지 않으면 주입 없이 기다립니다. direct dispatch resolver는
`glide_gate_interrupt_injected`일 때 ESP = 진입 + 4 − 12를 받아들입니다.

tick을 무장하는 코드(`PitIrqSchedule::Poll`, BIOS tick, JAMMA timeline, `timer_interrupt_pending`)는 호스트
폴 루프가 `ThreadContext::timer_tick_arm`에 등록하는 callable이고, 호스트 루프와 게스트의 대기 루프가
`timer_tick_arm_mutex` 아래에서 부릅니다. 호스트 루프는 present를 실행하는 스레드이므로 게스트가 기다리는
동안에는 게스트가 무장합니다. HLE 재진입(`HandleAotReentry`의 planner-HLE 분기)도 처리 뒤 주입을 시도해
`iret` 직후 밀린 tick이 중첩 없이 빠집니다(Task 747).

PIU10 MP3의 재생 시계(`Piu10Mp3AudioOut::Impl::PlaybackPosition`)는 자기 이전 값에서 PCM 속도로 진행하며
가져간 count − 장치 버퍼 절반을 목표로 4초 시정수(±2%)로 슬루합니다. 가져간 count를 넘지 않고 뒤로 가지
않습니다. frame-sync 토글과 decode 게이트가 이 시계로 구동됩니다.

# The swap wait and timer ticks, and the MP3 playback clock (Tasks 745–750)

On Linux x64 the `grBufferSwap` gate posts the present to the host thread (`PostBufferSwap`) and the
guest thread waits with `WaitForPendingSwaps`. The host thread's present is the driver's vsync or, when
the driver refused the swap interval, pacing (`PaceSwapAfterPresent`: deadlines accumulated from the
refresh rate, resynchronised when more than a period behind). When a tick is owed during the wait,
`ContinueGlideSwapWait` pushes an interrupt frame on the ESP with the return address popped, leaves EIP
at the address of the `call` that reached the gate and calls `InjectPendingInterrupts`. The ISR's
`iret` runs the call again, the gate sees `ThreadContext::glide_swap_wait_active` and keeps waiting,
and it returns to its caller only when the present is done. The return target is the call site rather
than the gate because the gate's segment (LINEXE 0x0080) has a descriptor not marked executable, which
the frame's CS lookup refuses. `FindGlideGateCallSite` verifies the call site as `call rel32` to the
gate or to an import thunk (`jmp rel32`, `jmp dword ptr [m32]`); unverified, the wait injects nothing.
The direct dispatch resolvers accept ESP = entry + 4 − 12 when `glide_gate_interrupt_injected` is set.

The code that arms ticks (`PitIrqSchedule::Poll`, the BIOS tick, the JAMMA timeline,
`timer_interrupt_pending`) is a callable the host poll loop registers as
`ThreadContext::timer_tick_arm`, run by the host loop and by the guest's wait loop under
`timer_tick_arm_mutex`. The host loop is the thread that runs the present, so while the guest waits it
is the guest that arms. An HLE reentry (`HandleAotReentry`'s planner-HLE branch) also attempts an
injection after handling, so owed ticks drain right after `iret` without nesting (Task 747).

The PIU10 MP3 playback clock (`Piu10Mp3AudioOut::Impl::PlaybackPosition`) advances from its own
previous value at the PCM rate and slews, with a four second time constant (±2%), to the pulled count
less half a device buffer. It never passes the pulled count and never runs backwards. The frame-sync
toggles and the decode gate are driven by it.

# 타이머 주입의 규칙, 입력 replay 프레임, 호스트 시계 (Tasks 751–754)

타이머 tick의 주입(`InjectPendingInterrupts`)은 다음을 차례로 확인하고, 하나라도 막으면 tick을 미룹니다.
(1) 호스트 컨텍스트의 IF. (2) 게스트의 `cli` hold(`GuestCliBlocksInjection`): user mode는 IF를 내릴 수 없으므로
`cli` HLE가 세우고 `sti` HLE나 주입 프레임을 꺼내는 `iret`(`NoteGuestIret`)가 내리며, 100 ms가 지나면
만료됩니다. (3) PIC의 in-service 모델(`PicTimerBlocksInjection`): 주입 프레임 스택(`handler_frames`)으로
중첩을 한 단계로 제한하고, 핸들러가 돌아오기 전의 `sti` 연쇄를 막습니다. (4) 반환 직후의 연쇄와 차례
(`PostReturnChainBlocksInjection`): 연쇄는 3개·주기 1 ms 이상, 인터럽트된 코드는 직전 핸들러가 쓴 시간만큼
차례를 받습니다. 주입한 뒤 핸들러로 들어갈 수 있는지는 `CanEnterTimerInterruptHandler`가 확인하고 Linux
x64는 cache로 재개합니다. direct 모델(Win32, Linux i386)은 게스트의 `iret`이 네이티브로 실행되므로 반환을
return pad로 봅니다(Task 762, 아래). 그 스위치를 끄면 반환이 보이지 않아 (3)의 중첩 제한과 (4)가 꺼집니다.
상태는 `PicTimerInService`(`pic_timer_in_service.h`)에 있습니다.

**타이머 핸들러의 복귀와 return pad (Task 762).** direct 모델에서 주입 frame의 복귀 EIP는 실제 주소가 아니라
return pad입니다: 커밋하지 않은 예약 페이지 한 장의 주소로, 거기로 `iret`하면 실행 폴트가 `EIP == pad`로
`DispatchGuestFault`의 맨 앞에 옵니다. 실제 복귀 주소와 그 frame의 ESP, 중단된 코드의 실행 상태
(`enable_single_step_trace`, `aot_legacy_fallback`, `aot_reentry_pending`)는 `TimerReturnPad`
(`timer_return_pad.h`)의 record에 있고, 핸들러는 그 상태 없이 돕니다. `HandleTimerReturnPad`가 record를
꺼내 EIP와 상태를 되돌리고, cache 모델의 `iret` HLE가 하는 기록(`NoteGuestIret`, `EndTimerInterrupt`)을 한 뒤
`kAfterHandlerReturn`으로 주입을 시도합니다. 복귀 주소가 제자리 게스트 코드면 `aot_reentry_pending`과 TF를
세워 다음 step에서 코드 캐시로 들여보냅니다. 제자리에는 safe point가 없어, 그대로 두면 트랩 없는 루프가
tick을 받지 못하기 때문입니다. **direct 모델에서 게스트를 제자리 주소로 재개하는 새 경로를 만들 때는 그 코드가
다음 tick을 받을 길이 있는지 확인하십시오.** `REPIU_TIMER_RETURN_PAD=0`이면 frame에 실제 주소를 넣는 이전
동작입니다. 자세한 것은 [Task 762 설계](docs/design/20260930-762-timer-handler-return-pad.md)에 있습니다.

입력 타임라인(`JammaInputTimeline`)은 tick이 주입될 때 그 tick의 예정 시각을 replay 프레임으로
쌓고(`BeginTimerInterrupt`), 입력 포트 읽기(`ReadJammaPort8`)에 그 시각의 키 상태를 돌려줍니다. 프레임은 세
가지로 회수됩니다: 읽기나 다음 주입의 ESP가 프레임보다 높을 때(스택 비교), 핸들러의 복귀가 그 ESP의
프레임을 꺼낼 때(`EndTimerInterrupt`, `HandleIretdInstruction`과 `HandleTimerReturnPad`에서 호출), 이후 주입이
`kReplayFrameStaleBegins`(64)를 넘었을 때(나이). 프레임이 없으면 읽기는 SDL의 키보드 상태로 가고, 입력
스크립트가 돌 때만 타임라인의 최신 상태로 갑니다(`ServeLiveReadsFromLatestState`; 밀어 넣은 이벤트는 SDL의
키보드 상태를 바꾸지 않음).

tick 스케줄(`PitIrqSchedule`)과 입력 타임라인의 시계는 `GlideOpenGlBackend::EventClockNanoseconds`이고
`steady_clock`을 돌려줍니다. MP3 재생 시계·swap 페이서·사운드 서버가 같은 시계(Linux의 `CLOCK_MONOTONIC`)를
따르기 때문입니다. SDL 이벤트의 timestamp는 SDL의 시계(Linux의 `CLOCK_MONOTONIC_RAW`) 값이므로
`TranslateEventTimestamp`가 이벤트의 나이를 재서 옮깁니다. 두 시계는 `HostClockDivergenceMeter`가 이벤트
펌프에서 1초 창으로 비교합니다(`event_clock.h`). **엔진에 새 시간 의존 경로를 넣을 때는 `steady_clock`을
쓰십시오**: 게스트가 음악과 맞춰야 하는 것이 SDL의 시계를 따르면 WSL2처럼 `CLOCK_MONOTONIC`이 slew되는
호스트에서 음악과 어긋납니다.

swap 페이서(`PaceSwapAfterPresent`)는 이 프레임의 마감(직전 마감 + 주기)보다 한 주기 넘게 늦었을 때만
재동기하고, 늦은 프레임은 자지 않습니다. 창을 열기 전에 `SelectWslD3d12Driver`가 WSL의 D3D12 드라이버를
고릅니다(`/dev/dxg`와 `d3d12_dri.so`가 있고 사용자가 드라이버를 고르지 않았을 때).

# The rules of timer injection, input replay frames, and the host clock (Tasks 751–754)

Injecting a timer tick (`InjectPendingInterrupts`) checks the following in order, and any of them defers
the tick. (1) IF in the host context. (2) The guest's `cli` hold (`GuestCliBlocksInjection`): user mode
cannot clear IF, so the `cli` HLE raises it and the `sti` HLE or the `iret` popping an injected frame
(`NoteGuestIret`) lowers it, and it expires after 100 ms. (3) The PIC's in-service model
(`PicTimerBlocksInjection`): the stack of injected frames (`handler_frames`) bounds nesting at one level,
and a chain at `sti` before the handler has returned is refused. (4) The chain after a return and the
turn (`PostReturnChainBlocksInjection`): a chain is at most 3 and at periods of 1 ms or more, and the
interrupted code gets a turn as long as the last handler took. Whether the handler can be entered after
an injection is checked by `CanEnterTimerInterruptHandler`, and Linux x64 resumes through the cache.
The direct model (Win32, Linux i386) runs the guest's `iret` natively and sees the return through the
return pad (Task 762, below); with that switch off the return is not seen, and the nesting bound of (3)
and all of (4) are off. The state lives in `PicTimerInService` (`pic_timer_in_service.h`).

**A timer handler's return and the return pad (Task 762).** On the direct model the return EIP of an
injected frame is not the real address but the return pad: the address of one reserved, uncommitted
page, so that an `iret` to it raises an execute fault that reaches the very top of `DispatchGuestFault`
with `EIP == pad`. The real return address, the frame's ESP and the interrupted code's execution state
(`enable_single_step_trace`, `aot_legacy_fallback`, `aot_reentry_pending`) wait in a record of
`TimerReturnPad` (`timer_return_pad.h`), and the handler runs without that state.
`HandleTimerReturnPad` takes the record, restores EIP and the state, records what the cache model's
`iret` HLE records (`NoteGuestIret`, `EndTimerInterrupt`) and attempts an injection as
`kAfterHandlerReturn`. When the return address is guest code in place it sets `aot_reentry_pending`
and TF, so that the next step enters the code cache: in place there is no safe point, and left there a
loop without a trap receives no tick. **When adding a path that resumes the guest at an address in
place on the direct model, check that the code has a way to receive the next tick.**
`REPIU_TIMER_RETURN_PAD=0` writes the real address into the frame as before. See the
[Task 762 design](docs/design/20260930-762-timer-handler-return-pad.md).

The input timeline (`JammaInputTimeline`) pushes a replay frame with a tick's due time when the tick is
injected (`BeginTimerInterrupt`) and answers input port reads (`ReadJammaPort8`) with the keys at that
time. A frame is retired in three ways: by a read's or the next injection's ESP being above it (the stack
test), by the handler's return popping the frame at its ESP (`EndTimerInterrupt`, called from
`HandleIretdInstruction` and `HandleTimerReturnPad`), and by more than `kReplayFrameStaleBegins` (64)
later injections (age). With
no frame a read goes to SDL's keyboard state, and to the timeline's latest state only while an input
script runs (`ServeLiveReadsFromLatestState`; pushed events do not change SDL's keyboard state).

The clock of the tick schedule (`PitIrqSchedule`) and of the input timeline is
`GlideOpenGlBackend::EventClockNanoseconds`, which returns `steady_clock`, because the MP3 playback
clock, the swap pacer and the sound server follow that clock (`CLOCK_MONOTONIC` on Linux). An SDL
event's timestamp is of SDL's clock (`CLOCK_MONOTONIC_RAW` on Linux), so `TranslateEventTimestamp` moves
it by the event's age. `HostClockDivergenceMeter` compares the two clocks in the event pump in windows of
a second (`event_clock.h`). **Use `steady_clock` for any new time-dependent path in the engine**:
whatever the guest must keep in time with the music parts from it, on a host that slews
`CLOCK_MONOTONIC` as WSL2 does, if it follows SDL's clock.

The swap pacer (`PaceSwapAfterPresent`) resynchronises only when more than a period behind this frame's
deadline (the previous deadline plus a period), and a late frame is not held. Before the window opens
`SelectWslD3d12Driver` chooses WSL's D3D12 driver (when `/dev/dxg` and `d3d12_dri.so` exist and the user
chose no driver).

# 실행 모델과 플랫폼 계층의 경계 (Task 759)

엔진·런타임·HLE·도구 디렉터리에는 플랫폼·아키텍처 하위 디렉터리가 없습니다. 호스트에 따라 달라지는 것은 두
곳으로 갑니다.

* **OS에 의존하는 것은 플랫폼 계층**(`include/repiu/platform/`의 계약, `src/platform/<OS>/[<아키텍처>/]`의
  구현)에 있습니다. 플랫폼 계층은 엔진의 타입을 쓰지 않습니다. 엔진이 호스트에 묻는 것 가운데 게스트 실행과
  무관한 것들: `host_crash_report.h`(처리되지 않은 예외 보고), `host_fault_report.h`(호스트 언어 예외 보고,
  페이지의 호스트 고유 번호, 폴트 보고용 메모리 읽기), `host_gpu_driver.h`(WSL의 D3D12 드라이버 선택),
  `host_symbols.h`(주소의 모듈·심볼 이름), `host_telemetry.h`(로더 이미지 범위, 스레드 시간, 다른 프로세스가
  만든 공유 섹션, 정지한 스레드의 레지스터), `host_trace_descriptor.h`(추적 덤프 쓰기),
  `host_thread.h`의 `TerminateHostThread`. Linux는 이 가운데 대부분에 "없음"으로 답합니다.
* **기계어 thunk와 스택 전환은 플랫폼 계층**에 있습니다. Win32는 MSVC 인라인 어셈블리
  (`src/platform/win32/aot_dbt_dispatch_thunks_win32.cpp`, `guest_stack_switch_win32.cpp`), Linux i386은
  `src/platform/linux/x86/`의 `.S`, Linux x64는 `src/platform/linux/x64/`의 `.S`입니다. 엔진의 resolver와
  스레드 컨텍스트는 C 심볼 이름으로 가리킵니다.
* **CPU 아키텍처에 따른 차이는 실행 모델**입니다. `direct`는 게스트의 바이트를 이 프로세스에서 그대로
  실행하고(Win32, Linux i386), `cache`는 번역한 long mode 코드 캐시를 실행하며(Linux x64), `emulated`는
  rex86 CPU 코어가 게스트를 실행하고(issue #61: 웹과 ARM 호스트용, Win32에서
  `-DREPIU_EXECUTION_MODEL_OVERRIDE=emulated`로 개발용 빌드), 웹 빌드도 `emulated`입니다(issue #67, 그 전까지는
  게스트를 실행하지 않는 `none`). 공용 코드는 `include/repiu/runtime/execution_model.h`의 함수
  (`RunsGuestBytesDirectly`, `RunsLongModeCodeCache`, `RunsGuestOnEmulatedCpu`, `RunsHostOnOneThread`,
  `LongModeReturnThunkAddress`,
  `InjectsTicksDuringSwapWait`)로 묻고, 구현은 `src/runtime/execution_model_{direct,cache,emulated}.cpp`입니다.
  CMake의 `REPIU_EXECUTION_MODEL`이 고릅니다.
* **모델에 따라 다르고 엔진의 타입이 필요한 코드**는 공용 파일 옆에 `<이름>_direct.cpp`,
  `<이름>_cache.cpp`, `<이름>_emulated.cpp`로 있습니다.

  | 공용 파일 | 내부 인터페이스 | 모델 파일이 맡는 것 |
  |---|---|---|
  | `execution/execution_trampoline.cpp` | `execution_trampoline_model.h` | direct: 게스트로의 두 진입. cache: 반환 thunk가 묻는 resolver, 캐시 진입, 캐시 출구, 종료 복구의 호스트 주소, 반환 추적. emulated: rex86 위의 진입, 멈춤을 `FaultEvent`로 바꾸는 것, 슬라이스 반복, 정지 요청 |
  | `aot/aot_dbt_*_dispatch.cpp` | 각 헤더의 `Get…ThunkAddress` | `aot_dbt_dispatch_thunks_direct.cpp`: 다섯 thunk의 주소. `_cache.cpp`: long mode Glide 게이트 thunk와 그 resolver. `_emulated.cpp`: 모두 null(생성 코드가 없음) |
  | `native_phase_sampler.cpp` | `native_phase_sampler_model.h` | 샘플을 어떻게 뜨는가. emulated는 뜨지 않음 |

  공용 파일에 남은 모델 분기는 `if (runtime::execution_model::RunsLongModeCodeCache())`로 감싼 같은
  코드입니다. 처리한 경계 뒤의 재개는 `ResumeHandledBoundary` 하나가 맡습니다.
* **호스트 루프는 두 모양입니다(issue #64).** 호스트가 하는 일 한 번(Glide 명령 펌프, 이벤트, PIT tick 일정,
  진행·정체 감시, 시간 제한, census)은 `src/engine/execution/host_turn.cpp`의 `HostTurn`입니다.
  `direct`·`cache`는 게스트 스레드를 따로 띄우고 메인 스레드의 `PollThreadUntilExit`가 "호스트 차례 → 기다림"을
  되풀이합니다. `emulated`는 `RunsHostOnOneThread()`가 참이라 게스트 스레드를 만들지 않습니다.
  `RunGuestOnThisThread`가 게스트 스레드 프로시저를 메인 스레드에서 부르고, rex86 루프가 멈춤 사이에 다음 tick
  기한이나 2 ms 상한이 되면 호스트 차례를 가집니다(`OneThreadHostLoop`). 오디오 출력 셋(YMZ280B, PIU10 MP3,
  CD 오디오)과 입력 스크립트는 한 단계(`Step`)를 스레드가 되풀이하거나, 한 스레드에서는 호스트 차례가
  `Pump()`로 부릅니다. HLE 쪽 대기는 `YieldToHost`로 하며, 한 스레드에서는 슬라이스를 끝냅니다. 브라우저의 단일
  Worker가 이 모양을 그대로 씁니다([설계 #64](docs/design/20261015-i064-browser-host-single-thread-loop.md)).

  ```mermaid
  flowchart LR
      subgraph threaded["direct · cache"]
          G1["게스트 스레드"] -- "Glide 랑데부" --> M1["메인: HostTurn → 기다림"]
          A1["오디오 워커 스레드: Step"]
      end
      subgraph one["emulated"]
          R["rex86 슬라이스"] --> T["HostTurn + 오디오 Pump"] --> R
      end
  ```

한 호스트에서만 만든 진단(`src/engine/telemetry/linux_x64_native_write_trace.cpp`, `aot_code_cache.cpp`의 스택
쓰기 추적)은 디렉터리로 나누지 않고 그 자리에서 그 호스트에서만 빌드됩니다.

**아직 엔진에 남은 Win32 코드**: `execution_trampoline.cpp`의 Win32 스레드 프로시저(SEH), 예외 진입
(`DispatchGuestException`, `exception_rescue_win32.cpp`), 종료 복구의 vectored handler들입니다. 모두 엔진의
상태를 쥐고 있어, 플랫폼 계층이 콜백을 받는 형태로 바꿔야 옮길 수 있습니다.

# The execution model and the boundary of the platform layer (Task 759)

The engine, runtime, HLE and tool directories have no platform or architecture subdirectories. What
differs by host goes to one of two places.

* **What depends on the OS is in the platform layer** (contracts in `include/repiu/platform/`,
  implementations in `src/platform/<OS>/[<arch>/]`). The platform layer uses none of the engine's types.
  What the engine asks of the host beyond running the guest: `host_crash_report.h` (the report of an
  unhandled exception), `host_fault_report.h` (the report of a host language exception, a page's own host
  numbers, memory reads for a fault report), `host_gpu_driver.h` (the choice of WSL's D3D12 driver),
  `host_symbols.h` (module and symbol names for addresses), `host_telemetry.h` (the loader's image range,
  thread times, a section another process created, the registers of a suspended thread),
  `host_trace_descriptor.h` (writing a trace dump) and `TerminateHostThread` in `host_thread.h`. Linux
  answers most of these with "none".
* **The machine-code thunks and the stack switch are in the platform layer.** Win32 has MSVC inline
  assembly (`src/platform/win32/aot_dbt_dispatch_thunks_win32.cpp`, `guest_stack_switch_win32.cpp`),
  Linux i386 the `.S` files of `src/platform/linux/x86/`, Linux x64 those of `src/platform/linux/x64/`.
  They name the engine's resolvers and thread context by their C symbols.
* **The difference by CPU architecture is the execution model.** `direct` runs the guest's bytes in this
  process as they are (Win32, Linux i386), `cache` runs the translated long-mode code cache (Linux x64),
  `emulated` has the rex86 CPU core run the guest (issue #61: for the web and ARM hosts, built on Win32
  for development with `-DREPIU_EXECUTION_MODEL_OVERRIDE=emulated`), and the web build is `emulated` too
  (issue #67; until then it was `none`, which ran no guest). Shared code asks through the functions of `include/repiu/runtime/execution_model.h`
  (`RunsGuestBytesDirectly`, `RunsLongModeCodeCache`, `RunsGuestOnEmulatedCpu`,
  `RunsHostOnOneThread`, `LongModeReturnThunkAddress`, `InjectsTicksDuringSwapWait`), implemented in
  `src/runtime/execution_model_{direct,cache,emulated}.cpp`. CMake's `REPIU_EXECUTION_MODEL` picks.
* **Code that differs by model and needs the engine's types** sits next to its shared file as
  `<name>_direct.cpp`, `<name>_cache.cpp` and `<name>_emulated.cpp`.

  | Shared file | Internal interface | What the model files hold |
  |---|---|---|
  | `execution/execution_trampoline.cpp` | `execution_trampoline_model.h` | direct: the two entries into the guest. cache: the resolver the return thunk asks, the entry into the cache, the cache's exit, the host address for shutdown recovery, the return traces. emulated: the entry on rex86, turning its stops into `FaultEvent`s, the slice loop, the stop request |
  | `aot/aot_dbt_*_dispatch.cpp` | `Get…ThunkAddress` in each header | `aot_dbt_dispatch_thunks_direct.cpp`: the five thunks' addresses. `_cache.cpp`: the long-mode Glide gate thunk and its resolver. `_emulated.cpp`: all null (no generated code) |
  | `native_phase_sampler.cpp` | `native_phase_sampler_model.h` | how a sample is taken; emulated takes none |

  The model branches left in shared files are the same code inside
  `if (runtime::execution_model::RunsLongModeCodeCache())`. The continuation after a handled boundary is
  one function, `ResumeHandledBoundary`.
* **The host loop has two shapes (issue #64).** One turn of the host's work (the Glide command pump,
  events, the PIT tick schedule, the progress and stall watch, the timeout, the censuses) is `HostTurn`
  in `src/engine/execution/host_turn.cpp`. `direct` and `cache` start a guest thread of its own, and
  the main thread's `PollThreadUntilExit` repeats "host turn, then wait". On `emulated`,
  `RunsHostOnOneThread()` holds and no guest thread is made: `RunGuestOnThisThread` calls the guest
  thread procedure on the main thread, and the rex86 loop takes a host turn between stops when the next
  tick is due or 2 ms have passed (`OneThreadHostLoop`). The three audio outputs (YMZ280B, PIU10 MP3,
  CD audio) and the input script have one `Step`, which a thread repeats, or which the host turn calls
  through `Pump()` on one thread. HLE-side waits go through `YieldToHost`, which on one thread ends the
  slice. The browser's single Worker uses this shape as it is
  ([design #64](docs/design/20261015-i064-browser-host-single-thread-loop.md)).

  ```mermaid
  flowchart LR
      subgraph threaded["direct · cache"]
          G1["guest thread"] -- "Glide rendezvous" --> M1["main: HostTurn, then wait"]
          A1["audio worker threads: Step"]
      end
      subgraph one["emulated"]
          R["rex86 slices"] --> T["HostTurn + audio Pump"] --> R
      end
  ```

A diagnostic built on one host (`src/engine/telemetry/linux_x64_native_write_trace.cpp`, the stack write
trace in `aot_code_cache.cpp`) is not put in a directory; it is built where it is, on that host only.

**Win32 code still in the engine**: the Win32 thread procedure (SEH) in `execution_trampoline.cpp`, the
exception entry (`DispatchGuestException`, `exception_rescue_win32.cpp`) and the shutdown recovery's
vectored handlers. All of them hold engine state, and they can move only once the platform layer takes
callbacks for them.
