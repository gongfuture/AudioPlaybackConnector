# 設備靠近自動重連（動態鎖思路）——設計與評估

日期：2026-09-12。分支：`feature1/approach-reconnect`。

## 目標（最初構想）

參考 Windows 動態鎖：設備遠離自動斷開、靠近自動重連；用 Classic RSSI（或
`RawSignalStrengthInDBm`）做距離探測，加滯回防抖動，並在啟動時檢測初狀態信號強度。

## 評估結論：RSSI 路線在純原生約束下不可行

| 候選 API | 結論 |
| --- | --- |
| Classic RSSI | 桌面行程沒有公開 API。查詢式 `WSALookupServiceBegin(BTH_QUERY_DEVICE)` 是一次 5-10 秒的阻塞 inquiry，耗電且節奏完全不可控；`IOCTL_BTH_GET_DEVICE_RSSI` 等內核路徑需要管理員/驅動 |
| LE `RawSignalStrengthInDBm` | 只有 `BluetoothLEAdvertisementWatcher` 能拿到，而手機平時（非配對模式）不發廣播，拿不到 |
| `System.Devices.Aep.SignalStrength`（AEP 屬性） | 屬性存在於屬性系統，但實測對藍牙 AEP 不填充（見下「實測」） |

結論：按「必須原生、寧可不裝額外內容」的約束，**距離探測 + 滯回不做**。

## 實現的子集：鏈路級「靠近自動重連」

動態鎖本身就是鏈路級的（手機藍牙鏈路斷了 = 人走了）。鏈路狀態有原生 API 且實測可用：

- `BluetoothDevice::GetDeviceSelector()` 是系統官方選擇器，桌面行程可以直接枚舉。
  注意手寫 `System.Devices.Aep.ProtocolId:="{e0cbf06c-...}"` 過濾器反而返回 0 結果——
  官方選擇器用的是 `System.Devices.DevObjectType:=5` 加上另一個 ProtocolId 變體
  `{E0CBF06C-CD8B-4647-BB8A-263B43F0F974}`，不要手寫。
- `System.Devices.Aep.IsConnected` 屬性可以隨 `DeviceWatcher` 的 Updated 事件拿到。

### 語義

- 設備**走遠**：系統會自行斷掉 A2DP 鏈路（這是平台行為），程式無需也不應該
  「主動斷開」——原構想的前半部分由作業系統完成。
- 設備**回來**：`IsConnected` false→true 時，若該設備是程式認得的（`lastDevices`
  裡有它的 MAC）、當前沒有會話、也沒有排在重連隊列裡，就排入
  `AUTORECONNECT` 計時器自動接回。
- **初狀態**：啟動時的既有連接由原有的 `WM_CONNECTDEVICE`（重連上次設備）負責；
  監視器只處理啟動之後的狀態翻轉，兩者不重疊。
- 「滯回」在鏈路級天然存在：鏈路建立/斷開本身就是系統遲滯後的離散事件，
  不會出現 RSSI 那種值抖動；應用層再加 `AUTORECONNECT_DELAY_MS` 去抖即可。

### 實現要點

- `SetupProximityWatcher()`：`DeviceWatcher(BluetoothDevice::GetDeviceSelector(),
  {System.Devices.Aep.IsConnected})`，Updated 回調只打包
  `PostPayload(WM_PROXIMITYCHANGED)`，判斷全在 UI 執行緒。
- AEP id 形如 `Bluetooth#Bluetooth<適配器>-<手機MAC冒號形式>`；lastDevices 存的
  是 BTHENUM 介面 id（MAC 為 12 位無冒號十六進制，`_C00000000` 之前）。兩邊各自
  歸一化成 12 位小寫十六進制對比，命中後**用 lastDevices 裡的 id** 去連
  （`AudioPlaybackConnection` 只認 BTHENUM 介面 id，不認 AEP id）。
- 右鍵菜單新增開關「Reconnect devices when they come back in range」
  （默認關），配置鍵 `autoReconnectOnApproach`。
- 一台手機可能在多個藍牙適配器下有多條 AEP（實測本機有兩條 Ace 5），
  按 MAC 歸一化後不會重複觸發。

### 實測

無線電開關模擬「離開→回來」：鏈路恢復後監視器觸發，worker 自動拉起，
全鏈路無人工干預（見提交資訊）。端點是否健康屬於
`research/bt-interrupt-no-sound` 分支研究的範疇（系統楔住時連接會無聲）。

## 遺留

- 若未來系統開放 AEP 信號強度（或專案接受 helper 進程/管理員方案），
  可以在 `WM_PROXIMITYCHANGED` 的判斷處插入滯回邏輯，介面不用動。
