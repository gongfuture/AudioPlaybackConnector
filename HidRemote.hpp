#pragma once

/* 藍牙 HID 媒體遙控器：把 PC 偽裝成 BLE Consumer Control 設備（HID 0x1812），
*  手機配對一次後，Windows 媒體鍵由本模組截獲並以 HID 報告發給手機，
*  Android 原生路由給當前媒體會話（後台也生效）。
*
*  為什麼要偽裝：Windows 藍牙棧作為 A2DP Sink + AVRCP CT 收得到手機的
*  元數據、也有可用的命令通道（絕對音量就是這麼發的），但沒有公開 API
*  （藍牙團隊在 Stack Overflow 72441918 的官方回覆確認不支持）。
*  HID 是唯一無需安裝、兩側都用原生功能的控制路徑。
*  詳見 docs/design/2026-09-12-media-control.md。
*
*  平台要求：藍牙適配器支持 LE Peripheral 角色（用
*  BluetoothAdapter::IsPeripheralRoleSupported 檢測，不支持則整體禁用）。
*  多設備：A2DP 全局一次只能連一台（平台限制），HID 轉發目標因此唯一；
*  其他手機即使也配對了遙控器，沒有 A2DP 會話就不會被轉發。 */

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

#include <chrono>
#include <thread>

size_t CountConnected();

namespace wdb = winrt::Windows::Devices::Bluetooth;
namespace wdba = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace wdbg = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;

// 遙控器在手機藍牙列表裡顯示的名字（廣播 LocalName，可自由命名）。
// 注意：配對完成後部分 Android 版本可能改顯示 PC 的系統藍牙名（跟主機名走）。
constexpr wchar_t HID_REMOTE_NAME[] = L"APC Remote";

// 開關狀態。改動一律在 UI 執行緒。
bool g_hidRemoteAvailable = false;   // 適配器支持 peripheral 且服務創建成功
bool g_hidSessionLive = false;       // 有手機（central）訂閱了報告特徵

static wdbg::GattServiceProvider g_hidProvider{ nullptr };
static wdbg::GattLocalService g_hidService{ nullptr };
static wdbg::GattLocalCharacteristic g_hidReportCharacteristic{ nullptr };
static winrt::event_token g_hidSubscribedToken{};
static HHOOK g_mediaKeyHook = nullptr;
static uint8_t g_hidReportValue = 0; // 當前報告位域

/* 消費控制 Report Map：Report ID 1，Next/Prev/Play-Pause/Stop/Mute 各 1 bit。
*  與市面藍牙遙控器（BLE HID consumer control）的標準描述符一致。 */
static constexpr uint8_t HID_REPORT_MAP[] = {
	0x05, 0x0C,       // Usage Page (Consumer)
	0x09, 0x01,       // Usage (Consumer Control)
	0xA1, 0x01,       // Collection (Application)
	0x85, 0x01,       //   Report ID (1)
	0x15, 0x00,       //   Logical Minimum (0)
	0x25, 0x01,       //   Logical Maximum (1)
	0x75, 0x01,       //   Report Size (1)
	0x95, 0x05,       //   Report Count (5)
	0x09, 0xB5,       //   Usage (Scan Next Track)
	0x09, 0xB6,       //   Usage (Scan Previous Track)
	0x09, 0xCD,       //   Usage (Play/Pause)
	0x09, 0xB7,       //   Usage (Stop)
	0x09, 0xE2,       //   Usage (Mute)
	0x81, 0x02,       //   Input (Data, Variable, Absolute) — 位域
	0x95, 0x03,       //   Report Count (3)
	0x81, 0x03,       //   Input (Const, Variable, Absolute) — 填充
	0xC0,             // End Collection
};
static constexpr uint8_t HID_INFORMATION[] = { 0x01, 0x01, 0x00, 0x00 }; // bcdHID 1.1, flags 0
static constexpr uint8_t HID_REPORT_REFERENCE[] = { 0x01, 0x01 };         // report id 1, input

// WH_KEYBOARD_LL 收到的虛擬鍵 → Report Map 裡的位（從 bit0 起：Next/Prev/Play/Stop/Mute）。
static uint8_t MediaKeyToBit(WPARAM vk)
{
	switch (vk)
	{
	case VK_MEDIA_NEXT_TRACK: return 0x01;
	case VK_MEDIA_PREV_TRACK: return 0x02;
	case VK_MEDIA_PLAY_PAUSE: return 0x04;
	case VK_MEDIA_STOP:       return 0x08;
	default:                  return 0x00;
	}
}

static void HidRemoteNotify(uint8_t bits)
{
	if (!g_hidReportCharacteristic)
		return;
	try
	{
		winrt::Windows::Storage::Streams::DataWriter writer;
		writer.WriteByte(0x01); // Report ID
		writer.WriteByte(bits);
		auto value = writer.DetachBuffer();
		for (auto const& session : g_hidReportCharacteristic.SubscribedClients())
		{
			try
			{
				g_hidReportCharacteristic.NotifyValueAsync(value, session);
			}
			catch (winrt::hresult_error const&)
			{
				LOG_CAUGHT_EXCEPTION(); // 單個客戶端失敗不影響其他
			}
		}
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

/* 按下→60ms→釋放。HID 是電平語義，沒有釋放報告手機會認為按鍵一直按著。
*  後台線程執行，不阻塞鍵盤鉤子；通知本身 fire-and-forget。 */
static winrt::fire_and_forget HidRemoteSendReport(uint8_t bits)
{
	g_hidReportValue = bits;
	HidRemoteNotify(bits);
	co_await winrt::resume_background();
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	g_hidReportValue = 0;
	HidRemoteNotify(0);
}

static LRESULT CALLBACK MediaKeyHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && wParam == WM_KEYDOWN)
	{
		// 只有「開關開、A2DP 連著設備、HID 有客戶端」時才攔截，其餘放行給本機播放器。
		if (g_forwardMediaKeys && g_hidSessionLive && CountConnected() > 0)
		{
			if (uint8_t bit = MediaKeyToBit(((KBDLLHOOKSTRUCT*)lParam)->vkCode))
			{
				HidRemoteSendReport(bit);
				return 1; // 攔下，不再交給本機 SMTC/播放器
			}
		}
	}
	return CallNextHookEx(g_mediaKeyHook, nCode, wParam, lParam);
}

void HidRemoteSetForwarding(bool on)
{
	g_forwardMediaKeys = on;
	if (!g_hidRemoteAvailable)
		return;

	if (on && !g_mediaKeyHook)
	{
		g_mediaKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, MediaKeyHookProc, g_hInst, 0);
		LOG_LAST_ERROR_IF(!g_mediaKeyHook);
	}
	else if (!on && g_mediaKeyHook)
	{
		UnhookWindowsHookEx(g_mediaKeyHook);
		g_mediaKeyHook = nullptr;
	}

	wdbg::GattServiceProviderAdvertisingParameters advParams;
	advParams.IsConnectable(true);
	try
	{
		if (on)
		{
			g_hidProvider.StartAdvertising(advParams);
			DebugLog(L"HidRemote: forwarding ON, connectable advertising started");
		}
		else
		{
			g_hidProvider.StopAdvertising();
			DebugLog(L"HidRemote: forwarding OFF, advertising stopped");
		}
	}
	catch (winrt::hresult_error const& e)
	{
		DebugLog(L"HidRemote: advertising failed 0x" + std::to_wstring(static_cast<uint32_t>(e.code().value)));
		LOG_CAUGHT_EXCEPTION();
	}
}

void ShutdownHidRemote()
{
	g_forwardMediaKeys = false;
	if (g_mediaKeyHook) { UnhookWindowsHookEx(g_mediaKeyHook); g_mediaKeyHook = nullptr; }
	g_hidService = nullptr;
	g_hidProvider = nullptr;
}

/* 讀請求統一應答。所有靜態值（Report Map、HID Information、Report Reference、
*  Report 的當前值）都在各自的 ReadRequested 事件裡給出。 */
static void HidRemoteServeRead(const wdbg::GattReadRequestedEventArgs& args, const uint8_t* data, size_t size)
{
	auto deferral = args.GetDeferral();
	try
	{
		auto request = args.GetRequestAsync().get();
		winrt::Windows::Storage::Streams::DataWriter writer;
		writer.WriteBytes(winrt::array_view<const uint8_t>(data, data + size));
		request.RespondWithValue(writer.DetachBuffer());
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
	deferral.Complete();
}

/* 創建 HID over GATT 服務並準備廣播。開關打開才廣播，避免常駐暴露。
*  注意：配對由手機發起（藍牙設置裡點「AudioPlayback Controller」），
*  Windows 側系統會彈配對確認，屬一次性操作。 */
void SetupHidRemote()
{
	try
	{
		auto adapter = wdb::BluetoothAdapter::GetDefaultAsync().get();
		if (!adapter.IsPeripheralRoleSupported())
			return; // 不支持 peripheral 角色的適配器（少見）直接禁用

		DebugLog(L"HidRemote: peripheral role supported, creating HID service...");
		auto providerResult = wdbg::GattServiceProvider::CreateAsync(
			wdb::BluetoothUuidHelper::FromShortId(0x1812)).get(); // HID service
		if (providerResult.Error() != wdb::BluetoothError::Success)
		{
			DebugLog(L"HidRemote: provider create failed (SIG UUID blocked?)");
			return; // 系統不允許創建 SIG 標準服務（個別版本），整體禁用
		}
		g_hidProvider = providerResult.ServiceProvider();
		auto service = g_hidProvider.Service();

		auto writer = [](const uint8_t* data, size_t size) {
			winrt::Windows::Storage::Streams::DataWriter w;
			w.WriteBytes(winrt::array_view<const uint8_t>(data, data + size));
			return w.DetachBuffer();
			};

		// Report Map（0x2A4B）：HID 描述符，決定 Android 把它識別成什麼設備（靜態值內嵌）
		wdbg::GattLocalCharacteristicParameters mapParams;
		mapParams.CharacteristicProperties(wdbg::GattCharacteristicProperties::Read);
		mapParams.StaticValue(writer(HID_REPORT_MAP, sizeof(HID_REPORT_MAP)));
		auto mapResult = service.CreateCharacteristicAsync(
			wdb::BluetoothUuidHelper::FromShortId(0x2A4B), mapParams).get();
		if (mapResult.Error() != wdb::BluetoothError::Success)
			return;

		// Report（0x2A4D）：讀 + 寫 + 通知，需加密（HOGP 規範要求）；值是動態的，走 ReadRequested
		wdbg::GattLocalCharacteristicParameters reportParams;
		reportParams.CharacteristicProperties(
			wdbg::GattCharacteristicProperties::Read |
			wdbg::GattCharacteristicProperties::Write |
			wdbg::GattCharacteristicProperties::Notify);
		reportParams.ReadProtectionLevel(wdbg::GattProtectionLevel::EncryptionRequired);
		reportParams.WriteProtectionLevel(wdbg::GattProtectionLevel::EncryptionRequired);
		auto reportResult = service.CreateCharacteristicAsync(
			wdb::BluetoothUuidHelper::FromShortId(0x2A4D), reportParams).get();
		if (reportResult.Error() != wdb::BluetoothError::Success)
			return;
		g_hidReportCharacteristic = reportResult.Characteristic();

		// Report Reference 描述符（0x2908）：[report id=1, type=input]（靜態值內嵌）
		wdbg::GattLocalDescriptorParameters refParams;
		refParams.StaticValue(writer(HID_REPORT_REFERENCE, sizeof(HID_REPORT_REFERENCE)));
		auto refResult = g_hidReportCharacteristic.CreateDescriptorAsync(
			wdb::BluetoothUuidHelper::FromShortId(0x2908), refParams).get();
		if (refResult.Error() != wdb::BluetoothError::Success)
			return;

		// HID Information（0x2A4A）與 HID Control Point（0x2A4C）
		wdbg::GattLocalCharacteristicParameters infoParams;
		infoParams.CharacteristicProperties(wdbg::GattCharacteristicProperties::Read);
		infoParams.StaticValue(writer(HID_INFORMATION, sizeof(HID_INFORMATION)));
		service.CreateCharacteristicAsync(
			wdb::BluetoothUuidHelper::FromShortId(0x2A4A), infoParams).get();

		wdbg::GattLocalCharacteristicParameters ctrlParams;
		ctrlParams.CharacteristicProperties(wdbg::GattCharacteristicProperties::WriteWithoutResponse);
		ctrlParams.WriteProtectionLevel(wdbg::GattProtectionLevel::Plain);
		service.CreateCharacteristicAsync(
			wdb::BluetoothUuidHelper::FromShortId(0x2A4C), ctrlParams).get();

		// Report 讀回：[report id, 當前位域]
		g_hidReportCharacteristic.ReadRequested([](const wdbg::GattLocalCharacteristic&, const wdbg::GattReadRequestedEventArgs& args) {
			uint8_t v[] = { 0x01, g_hidReportValue };
			HidRemoteServeRead(args, v, sizeof(v));
			});

		// 訂閱狀態 → g_hidSessionLive
		g_hidSubscribedToken = g_hidReportCharacteristic.SubscribedClientsChanged([](const wdbg::GattLocalCharacteristic& characteristic, const winrt::Windows::Foundation::IInspectable&) {
			g_hidSessionLive = characteristic.SubscribedClients().Size() > 0;
			});


		g_hidRemoteAvailable = true;
	}
	catch (winrt::hresult_error const& e)
	{
		LOG_HR(e.code());
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}
