# 설계: OSD에 현재 FPS와 최근 60초 그래프 (issue #88)

관련: [설계 #15 — OSD 배치](20261006-i015-osd-re2dj-layout.md), [#87 — 브라우저 60fps 상한](20261017-i087-browser-frame-cap.md)

## 배경

FPS는 지금 창 제목(Task 509)과 `REPIU_GLIDE_FRAME_RATE_LOG` 로그에만 나옵니다. 브라우저에는 창 제목이 없고, 전체 화면에서는
네이티브도 제목을 볼 수 없습니다. #87의 상한이 맞게 도는지, 어느 장면에서 떨어지는지를 게임 화면에서 바로 보고 싶습니다.

## 결정

1. **값은 백엔드의 1초 측정을 그대로 쓴다.** `RecordPresentedFrame`이 이미 1초마다 FPS를 계산합니다(제목과 로그의 값).
   그 값을 OSD에 넘깁니다(`GlideOsd::RecordFrameRate(fps, 걸린 초)`). OSD가 닫혀 있어도 기록합니다.
2. **기록은 1초 칸 60개.** 새 순수 클래스 `FrameRateHistory`(엔진). 한 번의 측정이 여러 초에 걸치면(로딩 중 화면 교체가
   없던 동안) 그 초 수만큼 같은 값을 채워, 그래프의 가로축이 늘 실제 60초입니다. 화면 교체가 하나도 없으면 측정이 없으므로,
   기록은 다음 교체 때 채워집니다.
3. **그리기는 별도의 작은 창, 오른쪽 위.** 기존 OSD 창(전체 폭, 위쪽)의 오른쪽 끝에 겹쳐 그립니다. 내용: `FPS 60.0`과
   `ImGui::PlotLines` 그래프(세로 0~max(65, 최근 최대)), 그 아래 최근 60초의 최소·평균·최대. 입력·초점·패드 탐색을 받지
   않습니다(`NoInputs`, `NoNav`, `NoFocusOnAppearing`), 그래서 기존 OSD 조작이 바뀌지 않습니다. 크기는 #15의 배율을 따릅니다.
4. 네이티브와 브라우저가 같은 코드입니다.

## 검증

* core probe에 `frame_rate_history`: 칸 채움, 여러 초 측정, 60칸 넘김, 최소·평균·최대.
* Win32 실행에서 OSD(Tab) 화면 캡처, 브라우저(headless Chrome)에서 Tab 뒤 캡처.

---

# Design: the current FPS and a graph of the last 60 seconds in the OSD (issue #88)

Related: [design #15 — the OSD layout](20261006-i015-osd-re2dj-layout.md), [#87 — the browser's 60 fps
cap](20261017-i087-browser-frame-cap.md)

## Background

The frame rate is shown only in the window title (Task 509) and the `REPIU_GLIDE_FRAME_RATE_LOG` log. The browser has no
window title, and fullscreen hides it natively as well. Whether #87's cap holds, and in which scenes the rate drops, should
be readable on the game screen.

## Decisions

1. **The value is the backend's one-second measurement.** `RecordPresentedFrame` already works out the rate every second
   (the title's and the log's value); it is passed to the OSD (`GlideOsd::RecordFrameRate(fps, seconds taken)`), also
   while the OSD is closed.
2. **The record is 60 one-second slots,** in a new pure engine class `FrameRateHistory`. A measurement spanning several
   seconds (a load with no swaps) fills that many slots with its value, so the graph's horizontal axis is always a real
   60 seconds. With no swap at all there is no measurement, and the slots are filled at the next swap.
3. **It is drawn in its own small window at the top right,** over the right end of the existing OSD window (full width,
   at the top): `FPS 60.0`, an `ImGui::PlotLines` graph (vertically 0 to max(65, the recent maximum)), and below it the
   minimum, mean and maximum of the last 60 seconds. It takes no input, focus or pad navigation (`NoInputs`, `NoNav`,
   `NoFocusOnAppearing`), so the existing OSD controls do not change. Its size follows #15's scale.
4. Native builds and the browser share the code.

## Verification

* `frame_rate_history` in the core probe: filling slots, a multi-second measurement, wrapping past 60, the minimum, mean
  and maximum.
* A capture of the OSD (Tab) in a Win32 run, and in the browser (headless Chrome) after Tab.
