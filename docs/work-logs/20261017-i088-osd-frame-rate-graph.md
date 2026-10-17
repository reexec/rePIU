# 작업 로그: OSD에 현재 FPS와 최근 60초 그래프 (issue #88)

설계: [20261017-i088](../design/20261017-i088-osd-frame-rate-graph.md) ·
작업 지시: [20261017-i088](../work-orders/20261017-i088-osd-frame-rate-graph.md)

## 2026-10-17 — 구현과 확인

브랜치 `task/i086-opfs-mount-cache`(#86, #87에 이어서).

### 구현

| 파일 | 내용 |
|---|---|
| `frame_rate_history.{h,cpp}` (새) | 1초 칸 60개. 여러 초 걸린 측정은 반올림한 초 수만큼(1~60) 채움. 최소·평균·최대 |
| `frame_rate_history_probe.cpp` (새) | core probe `frame_rate_history`: 빈 기록, 순서, 10초 공백, 60칸 넘김, 긴 공백·NaN |
| `glide_opengl_backend.cpp` | 1초 측정(제목·로그와 같은 값)마다 `RecordFrameRate`. OSD가 닫혀 있어도 기록 |
| `glide_osd.{h,cpp}` | 첫 부분을 두 칸 표로: 왼쪽 이름·렌더러, 오른쪽 `FPS 60.0`, 그래프, `FPS, last 60s: low … mean … high …` |

그래프는 처음에 `ImGui::PlotLines`로 했다가, 사용자 요청(60fps 기준선, 눈금, 바깥의 눈금 설명)으로 그리기 목록에 직접
그리도록 바꿨습니다. 60fps 기준선(초록), 30fps 간격 가로 눈금선, 10초 간격 세로 눈금선, 왼쪽 바깥에 FPS 값(60은 초록),
아래에 `-60s`·`-40s`·`-20s`·`now`. 세로 위끝은 max(75, 최근 최대×1.1), 200을 넘으면 눈금이 60 간격.
첫 그림에서 왼쪽 아래 `0`과 `-60s`가 겹쳐, 시간 글자를 그래프 폭 안에 두었습니다.

### 확인

| 대상 | 결과 |
|---|---|
| Win32 Release, pumpitea, Tab 20초 | 80초 캡처에 그래프. `FPS 60.0`, `low 41.5 mean 59.6 high 61.4`, 순간 하락이 보임 |
| 브라우저(headless Chrome), Tab 70초 | 140초 캡처에 그래프. `FPS 60.1`, 로딩 구간의 하락과 60 기준선 아래의 곡선(#87 상한) |
| core probe | Win32 Release·Debug·`emulated`, Linux x64·i386, Node 모두 통과(`frame_rate_history_all=true`) |

### 따로 본 것

* Win32 Release 실행 한 번이 시작 직후 게스트 예외로 끝났습니다(`0xC0000005`, 주소 `0xFFFFFFFF`, 창이 열린 뒤). 같은 바이너리로
  바로 다시 돌린 두 번은 80초까지 정상이었고, 바뀐 것은 호스트의 OSD 쪽뿐이라 이 변경과의 관련은 보이지 않습니다. 같은 실행에서
  OSD가 Tab 한 번에 세 번 열렸다 닫혔는데, 그 앞의 열림은 스크립트 키보다 먼저라 PC의 실제 입력(패드 조합)으로 보입니다.
  재현되면 별도 issue로.

### 남은 것

* 사용자 확인(네이티브·브라우저).

---

# Work log: the current FPS and a graph of the last 60 seconds in the OSD (issue #88)

Design: [20261017-i088](../design/20261017-i088-osd-frame-rate-graph.md) ·
Work order: [20261017-i088](../work-orders/20261017-i088-osd-frame-rate-graph.md)

## 2026-10-17 — implementation and checks

Branch `task/i086-opfs-mount-cache` (continuing after #86 and #87).

### Implementation

| File | What |
|---|---|
| `frame_rate_history.{h,cpp}` (new) | 60 one-second slots; a measurement that took several seconds fills its rounded seconds (1 to 60); minimum, mean, maximum |
| `frame_rate_history_probe.cpp` (new) | Core probe `frame_rate_history`: empty, order, a 10-second gap, wrapping past 60, a long gap and NaN |
| `glide_opengl_backend.cpp` | `RecordFrameRate` on every one-second measurement (the title's and log's value), also while the OSD is closed |
| `glide_osd.{h,cpp}` | The opening rows as a two-column table: the name and renderer on the left; `FPS 60.0`, the graph and `FPS, last 60s: low … mean … high …` on the right |

The graph was first `ImGui::PlotLines`; at the user's request (a 60 fps reference line, a scale, and the scale's labels
outside) it is drawn on the draw list instead: a 60 fps reference line (green), horizontal grid lines every 30 fps,
vertical ones every 10 seconds, the rate on the left outside (60 in green), and `-60s`, `-40s`, `-20s`, `now` under it.
The top is max(75, 1.1 × the recent maximum), with grid lines every 60 above 200. In the first picture the `0` and the
`-60s` at the bottom left overlapped, so the time labels are kept within the graph's width.

### Checks

| Target | Result |
|---|---|
| Win32 Release, pumpitea, Tab at 20 s | The graph in the 80 s capture: `FPS 60.0`, `low 41.5 mean 59.6 high 61.4`, brief dips visible |
| Browser (headless Chrome), Tab at 70 s | The graph in the 140 s capture: `FPS 60.1`, the dip during a load and the curve under the 60 line (#87's cap) |
| Core probe | Win32 Release, Debug and `emulated`, Linux x64 and i386, Node all pass (`frame_rate_history_all=true`) |

### Seen on the side

* One Win32 Release run ended right after start on a guest exception (`0xC0000005` at `0xFFFFFFFF`, after the window
  opened). Two runs of the same binary straight after reached 80 s normally, and the change is only in the host's OSD,
  so no link to it is visible. In that run the OSD opened and closed three times around one Tab, the first before the
  scripted key, which looks like real input on the PC (a pad chord). A separate issue if it comes back.

### Remaining

* The user's check (native and browser).
