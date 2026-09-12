# Windows 媒體鍵控制手機播放 / 手機媒體信息回傳 —— 評估結論：不可行

日期：2026-09-12。分支：`feature2/media-control`（本分支只留評估文檔，無代碼改動）。

## 目標（最初構想）

Windows 側媒體鍵通過藍牙傳遞到手機控制播放；同時把手機回傳的媒體信息
（曲目/歌手）顯示在 Windows 側。

## 實測過程與證據（2026-09-12 晚，真機 Ace 5 + adb）

1. 手機通過本程式的 `AudioPlaybackConnection` 連接，**A2DP SNK 端點 ACTIVE**，
   手機 `dumpsys audio` 顯示 `STREAM_MUSIC -> bt_a2dp(80)`——音頻確實在流向 PC。
2. 手機側 `dumpsys bluetooth_manager`：`Profile: AvrcpTargetService`、
   `AVRCP version: 1.6`——手機是 AVRCP Target，PC 是 Controller，元數據與
   播放控制通道確實存在於 Windows 藍牙棧內。
3. 但 Windows 側 `GlobalSystemMediaTransportControlsSessionManager`
   在整個過程中 **始終 0 個會話**（`probe.exe --smtc` 反覆確認）：
   系統不會把這種 sink 連接的 AVRCP 會話橋接進 SMTC，媒體鍵也因此
   不會路由到手機。
4. 曾嘗試的實現（會話橋 + 托盤顯示）因此永遠匹配不到會話，已整體回退。

## 結論

- 元數據和控制通道都存在於 Windows 藍牙棧（AvrcpTargetService/CT），
  但 **Windows 沒有任何公開 API 把它們暴露出來**：WinRT 的 SMTC 不橋接
  sink 連接的遠端會話；AVRCP 的 AVCTP 走 L2CAP 固定通道，歸棧所有，
  應用無法自行收發。
- 按約束（純原生、不引入額外內容），**兩個方向都不可實現**，故不做。
- 若未來 Windows 開放 sink 側 AVRCP 會話（或專案接受 helper 進程方案），
  本文件的評估可作為起點。
