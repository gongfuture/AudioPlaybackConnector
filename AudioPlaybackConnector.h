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

// 連線狀態一律在 UI 執行緒（訊息迴圈）上異動；WinRT 回呼與執行緒池回呼
// 都只負責 PostMessage 把 token 送回來，不碰任何共用狀態。
constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_DEVICESELECTED = WM_APP + 3;
constexpr UINT WM_DISCONNECTDEVICE = WM_APP + 4;
constexpr UINT WM_WORKERCONNECTED = WM_APP + 5;
constexpr UINT WM_WORKEREXITED = WM_APP + 6;

HANDLE g_hMutex = nullptr;
HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
Canvas g_xamlCanvas = nullptr;
Flyout g_xamlFlyout = nullptr;
MenuFlyout g_xamlMenu = nullptr;
FocusState g_menuFocusState = FocusState::Unfocused;
DevicePicker g_devicePicker = nullptr;

// 每條連線都跑在自己的 worker 行程裡。這不是為了容錯，而是 Windows 的硬限制：
// Windows_Media_Devices.dll 的 BluetoothA2dpPlaybackConnection::Start 會去註冊一個
// 模組全域的 TraceLogging provider，而 TraceLogging 對「已註冊」的 provider 會呼叫
// __fastfail(FAST_FAIL_INVALID_ARG) 直接砍掉行程。因此同一個行程裡不可能同時存在
// 兩條 AudioPlaybackConnection；行程隔離是唯一的解法（與 AppUserModelID 無關，
// 也不需要把 exe 複製成不同檔名）。連線拆除時 provider 會反註冊，所以一個 worker
// 服務完一條連線就結束即可。
enum class ConnectionState
{
	Connecting, // worker 已啟動，尚未回報連線成功
	Connected,  // worker 已回報連線成功
	Stopping,   // 使用者要求斷線，等待 worker 結束
};

// 一個裝置的連線狀態。g_audioPlaybackConnections 只能在 UI 執行緒上讀寫。
struct ConnectionEntry
{
	DeviceInformation device{ nullptr };
	uint64_t token = 0; // 用來丟棄「進行中但已被取代」的連線嘗試
	ConnectionState state = ConnectionState::Connecting;
};

// worker 用行程結束碼把結果帶回父行程。結束碼與 HRESULT 同樣是 32 位元，
// 所以協定就直接定成「結束碼是一個 HRESULT」：能對應到友善訊息的少數幾種原因
// 用下面的自訂值，其餘一律原封不動帶回真正的 HRESULT，父行程才有辦法顯示
// 「裝置被占用」這類具體原因，而不是一律 Unknown error。
//
// 自訂值設了 customer-defined bit (0x20000000)，系統 HRESULT 不會用這個位元，
// 因此不可能和真正的錯誤碼相撞。
constexpr HRESULT APC_S_CLOSED = S_OK;                              // 曾經連上，之後關閉或收到停止要求
constexpr HRESULT APC_E_CREATE_FAILED = static_cast<HRESULT>(0xA0000001);     // TryCreateFromId 回傳 null
constexpr HRESULT APC_E_REQUEST_TIMED_OUT = static_cast<HRESULT>(0xA0000002);
constexpr HRESULT APC_E_DENIED_BY_SYSTEM = static_cast<HRESULT>(0xA0000003);

// RegisterWaitForSingleObject 取得的 handle 不是用 CloseHandle 釋放的，wil 也只包了
// 新版 threadpool 的 PTP_WAIT，所以這裡自己補一個。UnregisterWaitEx 一定要傳
// INVALID_HANDLE_VALUE：那會等到進行中的回呼跑完才返回，這是「釋放 WorkerContext
// 之前不會有回呼還握著它」的唯一保證。只能在 UI 執行緒上解構（在回呼自己裡面呼叫
// 這個會死鎖）。
inline void UnregisterWaitBlocking(HANDLE waitHandle) noexcept
{
	UnregisterWaitEx(waitHandle, INVALID_HANDLE_VALUE);
}
using unique_registered_wait = wil::unique_any<HANDLE, decltype(&UnregisterWaitBlocking), UnregisterWaitBlocking>;

// 一個 worker 行程的所有 handle。只能在 UI 執行緒上建立與銷毀；
// 執行緒池回呼只讀 token 然後 PostMessage。
struct WorkerContext
{
	std::wstring deviceId;
	uint64_t token = 0;

	// 宣告順序是清理順序的一部分，不要重排：成員以宣告的相反順序解構，
	// 因此下面三個等待註冊一定先被解除（並等回呼結束），才輪到上面的 handle
	// 被關閉。反過來的話，回呼可能正拿著已經關掉的 handle。
	wil::unique_handle process;
	wil::unique_handle stopEvent;      // 父 -> 子：請正常結束
	wil::unique_handle connectedEvent; // 子 -> 父：連線已開啟
	unique_registered_wait connectedWait;
	unique_registered_wait processWait;
	unique_registered_wait stopTimeoutWait;
};

// C++/WinRT 的投影型別刻意刪除了 operator new，無法直接放到堆積上，
// 所以跨執行緒傳遞 DeviceInformation 時要包一層。
struct DevicePayload
{
	DeviceInformation device{ nullptr };
	explicit DevicePayload(const DeviceInformation& value) : device(value) {}
};

// worker 事件通知。必須帶 token：同一個裝置可能已經被斷開又重連，
// 舊 worker 遲來的通知不可以動到新的項目。
struct WorkerEventPayload
{
	uint64_t token = 0;
};

std::unordered_map<std::wstring, ConnectionEntry> g_audioPlaybackConnections;
std::unordered_map<uint64_t, std::unique_ptr<WorkerContext>> g_workers;
uint64_t g_nextConnectToken = 1;
HANDLE g_hJob = nullptr; // KILL_ON_JOB_CLOSE：父行程一消失，worker 一律跟著死
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
