# 작업 지시: OSD에 현재 FPS와 최근 60초 그래프 (issue #88)

설계: [20261017-i088](../design/20261017-i088-osd-frame-rate-graph.md)

1. `FrameRateHistory`(엔진, 순수): 1초 칸 60개, 여러 초 측정은 그 초 수만큼, 최소·평균·최대.
2. core probe `frame_rate_history`.
3. 백엔드: 1초 측정마다 `GlideOsd::RecordFrameRate`.
4. OSD: 첫 부분을 두 칸 표로, 오른쪽에 FPS 값과 그래프(60fps 기준선, 눈금선, 바깥 눈금 글자), 최소·평균·최대.
5. 검증: Win32 OSD 캡처, 브라우저 OSD 캡처, Win32·Linux·Node 빌드와 core probe.
6. 문서: 작업 로그.

---

# Work order: the current FPS and a graph of the last 60 seconds in the OSD (issue #88)

Design: [20261017-i088](../design/20261017-i088-osd-frame-rate-graph.md)

1. `FrameRateHistory` (engine, pure): 60 one-second slots, a multi-second measurement filling as many, the minimum, mean
   and maximum.
2. Core probe `frame_rate_history`.
3. The backend: `GlideOsd::RecordFrameRate` on every one-second measurement.
4. The OSD: the opening rows as a two-column table, the rate and the graph on the right (60 fps reference line, grid
   lines, scale labels outside), with the minimum, mean and maximum.
5. Verification: an OSD capture on Win32 and in the browser; Win32, Linux and Node builds and the core probe.
6. Documents: the work log.
