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

## 補充：耳機/車機是怎麼做到的（2026-09-12）

藍牙耳機和車機的做法就是標準 AVRCP CT/TG 模型：耳機/車機是 A2DP Sink +
**AVRCP Controller**，手機是 Source + Target。耳機上的按鍵由 CT 向手機發
passthrough 命令；曲目信息由手機（TG）通過 AVRCP 1.3+ 的元數據通知**主動推送**
給 CT。手機側原生支持，無需任何應用。

本程式下的 PC 角色與耳機完全一致（A2DP Sink + CT），數據也已經到達
Windows 藍牙棧——BthAvctpSvc.dll（AVCTP 服務）接收 AVRCP 元數據與命令通道。
但掃描該組件證實：其中沒有任何 SMTC / 對外接口字符串，元數據在棧內終止，
Windows 沒有把它交給應用的途徑。耳機/車機廠商是「整個棧自己實現」（車機
尤其如此）或使用棧廠商提供的 CT API，而 Windows 的 CT 對應用關閉。

## 可行的控制路徑（若接受 BLE HID 形態）

「控制手機」唯一無安裝可行的路：PC 用 GattServiceProvider 模擬 BLE HID
**Consumer Control** 設備（0x1812，Report Map 只含 Play/Pause/Next/Prev 等
消費控制碼，Appearance 用 Generic Remote 0x0180）——這不是「鍵盤」的形態，
而是市面上藍牙遙控器的標準品類。手機配對一次後，HID 報告裡的媒體鍵由
Android 原生路由給當前媒體會話（後台也生效）。截獲 Windows 媒體鍵用
WM_APPCOMMAND 全局鉤子，寫進 HID report 即可。

「顯示手機曲目」方向（手機 → PC）仍然沒有任何通道：AVRCP 元數據不出棧。
