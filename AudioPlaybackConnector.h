#pragma once

#include "resource.h"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::Data::Xml::Dom;
namespace fs = std::filesystem;

// 連線狀態一律在 UI 執行緒（訊息迴圈）上異動，WinRT 回呼只負責 PostMessage 把資料送回來。
constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_CONNECTIONCLOSED = WM_APP + 3;
constexpr UINT WM_DEVICESELECTED = WM_APP + 4;
constexpr UINT WM_DISCONNECTDEVICE = WM_APP + 5;
constexpr UINT WM_CONNECTRESULT = WM_APP + 6;

HANDLE g_hMutex = nullptr;
HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
Canvas g_xamlCanvas = nullptr;
Flyout g_xamlFlyout = nullptr;
MenuFlyout g_xamlMenu = nullptr;
FocusState g_menuFocusState = FocusState::Unfocused;
DevicePicker g_devicePicker = nullptr;

// 一個裝置的連線狀態。g_audioPlaybackConnections 只能在 UI 執行緒上讀寫。
struct ConnectionEntry
{
	DeviceInformation device{ nullptr };
	AudioPlaybackConnection connection{ nullptr };
	uint64_t token = 0; // 用來丟棄「進行中但已被取消」的連線嘗試
	bool connected = false;
};

// C++/WinRT 的投影型別刻意刪除了 operator new，無法直接放到堆積上，
// 所以跨執行緒傳遞 DeviceInformation 時要包一層。
struct DevicePayload
{
	DeviceInformation device{ nullptr };
	explicit DevicePayload(const DeviceInformation& value) : device(value) {}
};

// 連線被關閉的通知。必須帶上 token：同一個裝置可能已經被斷開又重連，
// 此時舊連線遲來的 Closed 事件不可以把新的項目清掉。
struct ClosedPayload
{
	std::wstring deviceId;
	uint64_t token = 0;
};

// 背景連線流程的結果，透過 WM_CONNECTRESULT 交回 UI 執行緒。
// 這裡刻意只帶原始狀態碼而不帶字串，因為 Translate() 的快取不是執行緒安全的。
struct ConnectResult
{
	std::wstring deviceId;
	uint64_t token = 0;
	AudioPlaybackConnection connection{ nullptr };
	bool created = true;
	bool success = false;
	AudioPlaybackConnectionOpenResultStatus status = AudioPlaybackConnectionOpenResultStatus::UnknownFailure;
	winrt::hresult error;
};

std::unordered_map<std::wstring, ConnectionEntry> g_audioPlaybackConnections;
uint64_t g_nextConnectToken = 1;
std::string g_notifyIconSvg;
HICON g_hTrayIcon = nullptr;
NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
UINT WM_TASKBAR_CREATED = 0;
bool g_reconnect = false;
bool g_showNotification = true;
std::vector<std::wstring> g_lastDevices;

#include "Util.hpp"
#include "FnvHash.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "Direct2DSvg.hpp"
