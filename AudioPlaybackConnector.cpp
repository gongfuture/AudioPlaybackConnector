#pragma warning(disable:4819)
#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
void ConnectDevice(const DeviceInformation& device);
winrt::fire_and_forget ConnectDeviceById(std::wstring deviceId);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();
bool GetStartupStatus();
void SetStartupStatus(bool status);
void ShowInitialToastNotification();
void SetDisplayStatusSafe(const DeviceInformation& device, std::wstring_view status, DevicePickerDisplayStatusOptions options);
std::wstring FormatWorkerError(DWORD exitCode);
size_t CountConnected();
bool TryGetArgValue(PCWSTR name, std::wstring& value);
int RunWorkerProcess(std::wstring_view deviceId, std::wstring_view stopEventName, std::wstring_view connectedEventName);
bool LaunchWorker(const std::wstring& deviceId, uint64_t token);
void DestroyWorker(uint64_t token);

// 把堆積上的資料交給 UI 執行緒處理。PostMessageW 是執行緒安全的。
// payload 以傳值方式接手所有權：
//   成功 -> 所有權移交給訊息佇列（WndProc 會用 unique_ptr 重新接管），
//           所以要 release() 放棄所有權，避免這裡把它刪掉造成 use-after-free；
//   失敗 -> 這則訊息不會有人收，payload 解構時記憶體就被釋放了。
template <typename T>
void PostPayload(UINT message, std::unique_ptr<T> payload)
{
	if (!PostMessageW(g_hWnd, message, reinterpret_cast<WPARAM>(payload.get()), 0))
	{
		LOG_LAST_ERROR();
		return;
	}
	payload.release();
}

// DevicePicker 關閉之後再呼叫 SetDisplayStatus 會拋例外，統一在這裡吞掉。
void SetDisplayStatusSafe(const DeviceInformation& device, std::wstring_view status, DevicePickerDisplayStatusOptions options)
{
	if (!g_devicePicker || !device)
	{
		return;
	}

	try
	{
		g_devicePicker.SetDisplayStatus(device, winrt::hstring(status), options);
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

size_t CountConnected()
{
	size_t count = 0;
	for (const auto& item : g_audioPlaybackConnections)
	{
		if (item.second.state == ConnectionState::Connected)
		{
			++count;
		}
	}
	return count;
}

bool TryGetArgValue(PCWSTR name, std::wstring& value)
{
	int argc = 0;
	auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv)
	{
		return false;
	}

	bool found = false;
	for (int i = 1; i < argc; ++i)
	{
		if (_wcsicmp(argv[i], name) == 0 && i + 1 < argc)
		{
			value = argv[i + 1];
			found = true;
			break;
		}
	}

	LocalFree(argv);
	return found;
}

// ---------------------------------------------------------------------------
// worker 行程本體：整個行程只服務一條連線，結束碼就是結果。
// ---------------------------------------------------------------------------
int RunWorkerProcess(std::wstring_view deviceId, std::wstring_view stopEventName, std::wstring_view connectedEventName)
{
	HRESULT result = E_FAIL;

	try
	{
		winrt::init_apartment();

		wil::unique_handle stopEvent(OpenEventW(SYNCHRONIZE, FALSE, std::wstring(stopEventName).c_str()));
		wil::unique_handle connectedEvent(OpenEventW(EVENT_MODIFY_STATE, FALSE, std::wstring(connectedEventName).c_str()));
		if (!stopEvent || !connectedEvent)
		{
			return LOG_HR(HRESULT_FROM_WIN32(GetLastError()));
		}

		auto connection = AudioPlaybackConnection::TryCreateFromId(deviceId);
		if (!connection)
		{
			return LOG_HR(APC_E_CREATE_FAILED);
		}

		wil::unique_handle closedEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		if (!closedEvent)
		{
			return LOG_HR(HRESULT_FROM_WIN32(GetLastError()));
		}

		const HANDLE closedHandle = closedEvent.get();
		connection.StateChanged([closedHandle](const auto& sender, const auto&) {
			if (sender.State() == AudioPlaybackConnectionState::Closed)
			{
				SetEvent(closedHandle);
			}
			});

		connection.StartAsync().get();
		const auto openResult = connection.OpenAsync().get();
		switch (openResult.Status())
		{
		case AudioPlaybackConnectionOpenResultStatus::Success:
			break;
		case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
			return LOG_HR(APC_E_REQUEST_TIMED_OUT);
		case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
			return LOG_HR(APC_E_DENIED_BY_SYSTEM);
		default:
		{
			// 把真正的失敗原因帶回去（例如裝置已被占用），別讓它變成一句 Unknown error。
			const auto extended = static_cast<HRESULT>(openResult.ExtendedError());
			return LOG_HR(extended != S_OK ? extended : E_FAIL);
		}
		}

		// 通知父行程連線已開啟。在這之前結束都會被父行程視為連線失敗。
		SetEvent(connectedEvent.get());

		HANDLE handles[2] = { closedHandle, stopEvent.get() };
		WaitForMultipleObjects(2, handles, FALSE, INFINITE);

		connection.Close();
		result = APC_S_CLOSED;
	}
	catch (...)
	{
		result = LOG_CAUGHT_EXCEPTION();
	}

	return result;
}

// ---------------------------------------------------------------------------
// 父行程這側的 worker 生命週期管理
// ---------------------------------------------------------------------------

// 以下三個回呼都在執行緒池執行緒上執行，只能 PostMessage，不可以碰任何共用狀態。
// context 的存活由 DestroyWorker 保證：它會先 UnregisterWaitEx 等待回呼結束才釋放。
void CALLBACK OnWorkerConnected(PVOID parameter, BOOLEAN)
{
	auto context = static_cast<WorkerContext*>(parameter);
	auto payload = std::make_unique<WorkerEventPayload>();
	payload->token = context->token;
	PostPayload(WM_WORKERCONNECTED, std::move(payload));
}

void CALLBACK OnWorkerExited(PVOID parameter, BOOLEAN)
{
	auto context = static_cast<WorkerContext*>(parameter);
	auto payload = std::make_unique<WorkerEventPayload>();
	payload->token = context->token;
	PostPayload(WM_WORKEREXITED, std::move(payload));
}

// 只有在使用者要求斷線後才註冊：worker 若賴著不走就強制終止，
// 否則它會一直佔著那個裝置的 A2DP 連線，該裝置就再也連不上了。
void CALLBACK OnWorkerStopTimeout(PVOID parameter, BOOLEAN timerOrWaitFired)
{
	if (!timerOrWaitFired)
	{
		return; // worker 自己結束了，不需要動手
	}
	auto context = static_cast<WorkerContext*>(parameter);
	TerminateProcess(context->process.get(), static_cast<UINT>(APC_S_CLOSED));
}

bool LaunchWorker(const std::wstring& deviceId, uint64_t token)
{
	auto context = std::make_unique<WorkerContext>();
	context->deviceId = deviceId;
	context->token = token;

	const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(token);
	const auto stopName = L"Local\\AudioPlaybackConnector_Stop_" + suffix;
	const auto connectedName = L"Local\\AudioPlaybackConnector_Connected_" + suffix;

	context->stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, stopName.c_str()));
	context->connectedEvent.reset(CreateEventW(nullptr, TRUE, FALSE, connectedName.c_str()));
	if (!context->stopEvent || !context->connectedEvent)
	{
		LOG_LAST_ERROR();
		return false;
	}

	// 用同一份 exe，不再複製檔案：需要的只是行程隔離。
	auto commandLine = L"\"" + GetModuleFsPath(g_hInst).wstring() + L"\" --worker \"" + deviceId +
		L"\" --stopEvent \"" + stopName + L"\" --connectedEvent \"" + connectedName + L"\"";

	STARTUPINFOW startupInfo = { sizeof(startupInfo) };
	PROCESS_INFORMATION processInfo = {};
	if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo))
	{
		LOG_LAST_ERROR();
		return false;
	}
	CloseHandle(processInfo.hThread);
	context->process.reset(processInfo.hProcess);

	if (g_hJob)
	{
		LOG_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(g_hJob, context->process.get()));
	}

	// 兩個非同步等待取代原本的阻塞輪詢：連線成功一個、行程結束一個。
	auto raw = context.get();
	if (!RegisterWaitForSingleObject(context->connectedWait.put(), context->connectedEvent.get(), OnWorkerConnected, raw, INFINITE, WT_EXECUTEONLYONCE) ||
		!RegisterWaitForSingleObject(context->processWait.put(), context->process.get(), OnWorkerExited, raw, INFINITE, WT_EXECUTEONLYONCE))
	{
		LOG_LAST_ERROR();
		g_workers.emplace(token, std::move(context));
		DestroyWorker(token); // 走一般清理路徑，順便終止已啟動的 worker
		return false;
	}

	g_workers.emplace(token, std::move(context));
	return true;
}

// 只在 UI 執行緒呼叫。
void DestroyWorker(uint64_t token)
{
	auto it = g_workers.find(token);
	if (it == g_workers.end())
	{
		return;
	}
	// 還沒結束的 worker 直接終止。它一結束 processWait 會再送一則 WM_WORKEREXITED，
	// 但那時 g_workers 裡已經沒有這個 token，那則訊息會被忽略。
	auto& worker = *it->second;
	if (worker.process && WaitForSingleObject(worker.process.get(), 0) != WAIT_OBJECT_0)
	{
		TerminateProcess(worker.process.get(), static_cast<UINT>(APC_S_CLOSED));
	}

	// 其餘清理交給 WorkerContext 的解構子：等待註冊會先被解除（並等回呼跑完），
	// 才輪到 handle 被關閉，順序由成員宣告順序保證。
	g_workers.erase(it);
}

// worker 的結束碼就是一個 HRESULT。已知原因給友善訊息，其餘一律把系統的
// 錯誤描述和原始碼值一起顯示出來，使用者才看得出是「裝置已被占用」還是別的問題。
// 只在 UI 執行緒呼叫。
std::wstring FormatWorkerError(DWORD exitCode)
{
	const auto hr = static_cast<HRESULT>(exitCode);
	switch (hr)
	{
	case APC_E_REQUEST_TIMED_OUT:
		return _(L"The request timed out");
	case APC_E_DENIED_BY_SYSTEM:
		return _(L"The operation was denied by the system");
	case APC_E_CREATE_FAILED:
		return _(L"Unknown error");
	default:
		break;
	}

	// STILL_ACTIVE 代表 GetExitCodeProcess 抓到的不是真正的結束碼，別拿它去解讀。
	if (SUCCEEDED(hr) || hr == static_cast<HRESULT>(STILL_ACTIVE))
	{
		return _(L"Unknown error");
	}

	// 只有錯誤碼需要格式化，長度固定（" (0xXXXXXXXX)" 共 13 個字元），訊息本身
	// 直接串接就好，不必猜緩衝區大小。swprintf 對「緩衝區不足」和「編碼錯誤」
	// 都回傳負值、分不出來，用它的回傳值去擴張緩衝區會在真的格式錯誤時無限成長。
	wchar_t code[16] = {};
	swprintf_s(code, L" (0x%08X)", static_cast<uint32_t>(hr));
	return std::wstring(winrt::hresult_error(hr).message()) + code;
}

bool IsSystemLightTheme()
{
	wil::unique_hkey hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
	{
		DWORD value = 1;
		DWORD size = sizeof(value);
		DWORD type = REG_DWORD;
		if (RegQueryValueExW(hKey.get(), L"SystemUsesLightTheme", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD)
		{
			return value != 0;
		}
	}
	return false;
}

HICON CreateNotifyIcon(size_t connectionCount)
{
	if (g_notifyIconSvg.empty())
	{
		return nullptr;
	}

	const int width = GetSystemMetrics(SM_CXSMICON);
	const int height = GetSystemMetrics(SM_CYSMICON);
	const bool hasConnection = connectionCount > 0;
	D2D1_COLOR_F baseColor = hasConnection ? D2D1::ColorF(0.0f, 0.47f, 1.0f, 1.0f) : (IsSystemLightTheme() ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));

	wil::unique_hdc hdc(CreateCompatibleDC(nullptr));
	THROW_IF_NULL_ALLOC(hdc);

	auto hBitmap = CreateDIB(hdc.get(), width, height, 32);
	THROW_IF_NULL_ALLOC(hBitmap);
	auto hBitmapMask = CreateDIB(hdc.get(), width, height, 1);
	THROW_IF_NULL_ALLOC(hBitmapMask);

	auto select = wil::SelectObject(hdc.get(), hBitmap.get());
	DrawSvgTohDC(g_notifyIconSvg, hdc.get(), width, height, baseColor);

	if (hasConnection)
	{
		const int badgeRadius = max(4, width / 4);
		const int margin = 1;
		const int cx = width - badgeRadius - margin;
		const int cy = height - badgeRadius - margin;
		RECT badgeRect = { cx - badgeRadius, cy - badgeRadius, cx + badgeRadius, cy + badgeRadius };

		wil::unique_hbrush badgeBrush(CreateSolidBrush(RGB(0, 120, 215)));
		auto oldBrush = SelectObject(hdc.get(), badgeBrush.get());
		auto oldPen = SelectObject(hdc.get(), GetStockObject(NULL_PEN));
		Ellipse(hdc.get(), badgeRect.left, badgeRect.top, badgeRect.right, badgeRect.bottom);
		SelectObject(hdc.get(), oldPen);
		SelectObject(hdc.get(), oldBrush);

		std::wstring text = connectionCount > 99 ? L"99+" : std::to_wstring(connectionCount);
		HFONT font = CreateFontW(-MulDiv(7, GetDeviceCaps(hdc.get(), LOGPIXELSY), 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
		if (font)
		{
			auto oldFont = SelectObject(hdc.get(), font);
			SetTextColor(hdc.get(), RGB(255, 255, 255));
			SetBkMode(hdc.get(), TRANSPARENT);
			DrawTextW(hdc.get(), text.c_str(), -1, &badgeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			SelectObject(hdc.get(), oldFont);
			DeleteObject(font);
		}

		DIBSECTION dib = {};
		if (GetObjectW(hBitmap.get(), sizeof(dib), &dib) == sizeof(dib) && dib.dsBm.bmBits)
		{
			auto pixels = static_cast<uint32_t*>(dib.dsBm.bmBits);
			const int pixelCount = dib.dsBm.bmWidth * abs(dib.dsBm.bmHeight);
			for (int i = 0; i < pixelCount; ++i)
			{
				if ((pixels[i] & 0xFF000000u) == 0 && (pixels[i] & 0x00FFFFFFu) != 0)
				{
					pixels[i] |= 0xFF000000u;
				}
			}
		}
	}

	ICONINFO iconInfo = {
		.fIcon = TRUE,
		.hbmMask = hBitmapMask.get(),
		.hbmColor = hBitmap.get()
	};

	HICON hIcon = CreateIconIndirect(&iconInfo);
	THROW_LAST_ERROR_IF_NULL(hIcon);
	return hIcon;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	// worker 模式：同一份 exe，只是換一組參數。必須擋在單一實例檢查之前。
	std::wstring workerDeviceId;
	if (TryGetArgValue(L"--worker", workerDeviceId))
	{
		std::wstring stopEventName, connectedEventName;
		if (!TryGetArgValue(L"--stopEvent", stopEventName) || !TryGetArgValue(L"--connectedEvent", connectedEventName))
		{
			return E_INVALIDARG;
		}
		return RunWorkerProcess(workerDeviceId, stopEventName, connectedEventName);
	}

	// Prevent multiple instances
	g_hMutex = CreateMutexW(nullptr, FALSE, L"Local\\AudioPlaybackConnector_Mutex");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		if (g_hMutex)
		{
			CloseHandle(g_hMutex);
			g_hMutex = nullptr;
		}
		TaskDialog(nullptr, nullptr, _(L"Already running!"), nullptr, _(L"AudioPlaybackConnector is already running in background.\r\nCheck system tray."), TDCBF_OK_BUTTON, TD_WARNING_ICON, nullptr);
		return EXIT_FAILURE;
	}

	g_hInst = hInstance;

	// 父行程若異常結束（當掉、被工作管理員結束），worker 一律跟著被殺，不留孤兒。
	g_hJob = CreateJobObjectW(nullptr, nullptr);
	if (g_hJob)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits = {};
		jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		LOG_IF_WIN32_BOOL_FALSE(SetInformationJobObject(g_hJob, JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)));
	}
	else
	{
		LOG_LAST_ERROR();
	}

	winrt::init_apartment();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadTranslateData();
	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	if (g_showNotification)
	{
		ShowInitialToastNotification();
	}

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
	{
		if (g_reconnect)
		{
			SaveSettings();
		}

		// 先送出停止要求，讓剛好來得及的 worker 自己收尾。這裡不等它們：
		// 下面的 DestroyWorker 會把還在執行的直接 TerminateProcess，
		// 所以實務上多數 worker 是被硬砍的。可以接受——行程結束時核心會關閉
		// 它所有的 handle，音訊服務一樣會察覺客戶端消失並拆掉 A2DP 連線，
		// 這也正是原本 worker 設計依賴的機制。
		// （關閉 job 是另一層保險，針對的是父行程異常死亡、跑不到這裡的情況。）
		for (auto& worker : g_workers)
		{
			if (worker.second->stopEvent)
			{
				SetEvent(worker.second->stopEvent.get());
			}
		}

		auto connections = std::move(g_audioPlaybackConnections);
		g_audioPlaybackConnections.clear();
		for (auto& item : connections)
		{
			SetDisplayStatusSafe(item.second.device, {}, DevicePickerDisplayStatusOptions::None);
		}

		std::vector<uint64_t> tokens;
		tokens.reserve(g_workers.size());
		for (const auto& worker : g_workers)
		{
			tokens.push_back(worker.first);
		}
		for (auto token : tokens)
		{
			DestroyWorker(token);
		}

		if (g_hJob) { CloseHandle(g_hJob); g_hJob = nullptr; }

		if (!g_reconnect)
		{
			SaveSettings();
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		if (g_hTrayIcon) { DestroyIcon(g_hTrayIcon); g_hTrayIcon = nullptr; }
		if (g_hMutex) { CloseHandle(g_hMutex); g_hMutex = nullptr; }
		PostQuitMessage(0);
	}
	break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
			SetForegroundWindow(hWnd);
			g_devicePicker.Show(rect, Placement::Above);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_DEVICESELECTED:
	{
		auto payload = std::unique_ptr<DevicePayload>(reinterpret_cast<DevicePayload*>(wParam));
		ConnectDevice(payload->device);
	}
	break;
	case WM_DISCONNECTDEVICE:
	{
		/* 使用者主動要求斷線。這裡不阻塞等待 worker 結束——只送出停止要求並把項目標成
		*  Stopping，真正的清理留給 WM_WORKEREXITED。項目刻意不立刻移除，否則使用者在
		*  worker 還沒退出前又點一次連線，會出現兩個 worker 搶同一個裝置。 */
		auto payload = std::unique_ptr<DevicePayload>(reinterpret_cast<DevicePayload*>(wParam));
		auto it = g_audioPlaybackConnections.find(std::wstring(payload->device.Id()));
		if (it == g_audioPlaybackConnections.end())
		{
			// 沒有對應項目（重複點擊、或狀態已被清掉），把 picker 的顯示歸零就好。
			SetDisplayStatusSafe(payload->device, {}, DevicePickerDisplayStatusOptions::None);
			UpdateNotifyIcon();
			break;
		}

		if (it->second.state != ConnectionState::Stopping)
		{
			it->second.state = ConnectionState::Stopping;

			auto worker = g_workers.find(it->second.token);
			if (worker != g_workers.end())
			{
				if (worker->second->stopEvent)
				{
					SetEvent(worker->second->stopEvent.get());
				}
				// worker 若卡住不退出就在逾時後強制終止，否則它會一直佔著該裝置。
				if (!worker->second->stopTimeoutWait && worker->second->process)
				{
					LOG_IF_WIN32_BOOL_FALSE(RegisterWaitForSingleObject(worker->second->stopTimeoutWait.put(),
						worker->second->process.get(), OnWorkerStopTimeout, worker->second.get(),
						3000, WT_EXECUTEONLYONCE));
				}
			}
		}

		// 顯示進度並拿掉斷線按鈕：worker 結束前這個裝置不能再操作，
		// 沒有這個提示的話使用者再點下去會完全沒有反應。狀態會在 WM_WORKEREXITED 清掉。
		SetDisplayStatusSafe(it->second.device, _(L"Disconnecting"), DevicePickerDisplayStatusOptions::ShowProgress);
		UpdateNotifyIcon();
	}
	break;
	case WM_WORKERCONNECTED:
	{
		auto payload = std::unique_ptr<WorkerEventPayload>(reinterpret_cast<WorkerEventPayload*>(wParam));
		auto worker = g_workers.find(payload->token);
		if (worker == g_workers.end())
		{
			break;
		}
		auto it = g_audioPlaybackConnections.find(worker->second->deviceId);
		// token 不符代表這個項目已經被後來的連線嘗試取代，這則通知該丟棄。
		if (it != g_audioPlaybackConnections.end() && it->second.token == payload->token &&
			it->second.state == ConnectionState::Connecting)
		{
			it->second.state = ConnectionState::Connected;
			SetDisplayStatusSafe(it->second.device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
			UpdateNotifyIcon();
		}
	}
	break;
	case WM_WORKEREXITED:
	{
		/* worker 結束的原因有三種，靠項目當下的狀態區分：
		*  Connecting -> 從未連上，是連線失敗，要顯示原因與重試按鈕；
		*  Connected  -> 曾經連上但非使用者觸發（藍牙走遠、系統關閉連線等），靜靜清掉；
		*  Stopping   -> 使用者主動斷線後的正常結束。 */
		auto payload = std::unique_ptr<WorkerEventPayload>(reinterpret_cast<WorkerEventPayload*>(wParam));
		auto worker = g_workers.find(payload->token);
		if (worker == g_workers.end())
		{
			break;
		}

		const auto deviceId = worker->second->deviceId;
		DWORD exitCode = static_cast<DWORD>(E_FAIL);
		if (worker->second->process)
		{
			LOG_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(worker->second->process.get(), &exitCode));
		}
		DestroyWorker(payload->token);

		auto it = g_audioPlaybackConnections.find(deviceId);
		if (it == g_audioPlaybackConnections.end() || it->second.token != payload->token)
		{
			break;
		}

		const auto state = it->second.state;
		auto device = it->second.device;
		g_audioPlaybackConnections.erase(it);

		if (state == ConnectionState::Connecting)
		{
			SetDisplayStatusSafe(device, FormatWorkerError(exitCode), DevicePickerDisplayStatusOptions::ShowRetryButton);
		}
		else
		{
			SetDisplayStatusSafe(device, {}, DevicePickerDisplayStatusOptions::None);
		}
		UpdateNotifyIcon();
	}
	break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				ConnectDeviceById(i);
			}
			g_lastDevices.clear();
		}
		break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
		});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
		});

	FontIcon checkedIcon, uncheckedIcon;
	checkedIcon.Glyph(L"\xE73E");

	MenuFlyoutItem startupItem;
	startupItem.Text(_(L"Run at login"));
	if (GetStartupStatus()) {
		startupItem.Icon(checkedIcon);
	}
	else {
		startupItem.Icon(uncheckedIcon);
	}
	startupItem.Click([checkedIcon, uncheckedIcon](const auto& sender, const auto&) {
		MenuFlyoutItem self = sender.as<MenuFlyoutItem>();
		if (GetStartupStatus()) {
			SetStartupStatus(false);
			self.Icon(uncheckedIcon);
		}
		else {
			SetStartupStatus(true);
			self.Icon(checkedIcon);
		}
		});

	FontIcon notificationCheckedIcon, notificationUncheckedIcon;
	notificationCheckedIcon.Glyph(L"\xE73E");

	MenuFlyoutItem notificationItem;
	notificationItem.Text(_(L"Show startup notification"));
	if (g_showNotification) {
		notificationItem.Icon(notificationCheckedIcon);
	}
	else {
		notificationItem.Icon(notificationUncheckedIcon);
	}
	notificationItem.Click([notificationCheckedIcon, notificationUncheckedIcon](const auto& sender, const auto&) {
		MenuFlyoutItem self = sender.as<MenuFlyoutItem>();
		g_showNotification = !g_showNotification;
		if (g_showNotification) {
			self.Icon(notificationCheckedIcon);
		}
		else {
			self.Icon(notificationUncheckedIcon);
		}
		SaveSettings();
		});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
		});

	MenuFlyout menu;
	menu.Items().Append(settingsItem);
	menu.Items().Append(startupItem);
	menu.Items().Append(notificationItem);
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
		});
	menu.Closed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
		});

	g_xamlMenu = menu;
}

// 只在 UI 執行緒呼叫。
void ConnectDevice(const DeviceInformation& device)
{
	auto deviceId = std::wstring(device.Id());

	auto [it, inserted] = g_audioPlaybackConnections.try_emplace(deviceId);
	if (!inserted)
	{
		switch (it->second.state)
		{
		case ConnectionState::Connected:
			SetDisplayStatusSafe(it->second.device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
			break;
		case ConnectionState::Connecting:
			SetDisplayStatusSafe(it->second.device, _(L"Connecting"),
				DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);
			break;
		case ConnectionState::Stopping:
			// 前一個 worker 還在收尾，此時再開一個會有兩個行程搶同一個裝置。
			// 維持「中斷連線中」的提示，等 WM_WORKEREXITED 清掉項目後才能重連。
			SetDisplayStatusSafe(it->second.device, _(L"Disconnecting"), DevicePickerDisplayStatusOptions::ShowProgress);
			break;
		}
		return;
	}

	const auto token = g_nextConnectToken++;
	it->second.device = device;
	it->second.token = token;
	it->second.state = ConnectionState::Connecting;

	SetDisplayStatusSafe(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	if (!LaunchWorker(deviceId, token))
	{
		g_audioPlaybackConnections.erase(it);
		SetDisplayStatusSafe(device, _(L"Unknown error"), DevicePickerDisplayStatusOptions::ShowRetryButton);
		return;
	}
	UpdateNotifyIcon();
}

// 開機重連只用得到 deviceId，解析成 DeviceInformation 之後丟回 UI 執行緒走一般流程。
winrt::fire_and_forget ConnectDeviceById(std::wstring deviceId)
{
	try
	{
		auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
		PostPayload(WM_DEVICESELECTED, std::make_unique<DevicePayload>(device));
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}


void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		// 一併把 topmost 拿掉：只做 SWP_HIDEWINDOW 會讓 WS_EX_TOPMOST 一直留著。
		SetWindowPos(g_hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
		});
	// 以下兩個回呼可能在任意執行緒上被觸發，而且是在 picker 內部的呼叫堆疊裡。
	// 只做 PostMessage 立刻返回，絕不在這裡面做任何阻塞或回呼 picker 的動作，
	// 否則使用者一點視窗外面就會和 picker 的 dismiss 流程互等而死鎖。
	g_devicePicker.DeviceSelected([](const auto&, const auto& args) {
		PostPayload(WM_DEVICESELECTED, std::make_unique<DevicePayload>(args.SelectedDevice()));
		});
	g_devicePicker.DisconnectButtonClicked([](const auto&, const auto& args) {
		PostPayload(WM_DISCONNECTDEVICE, std::make_unique<DevicePayload>(args.Device()));
		});
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	g_notifyIconSvg.assign(svgData, size);
}

void UpdateNotifyIcon()
{
	auto icon = CreateNotifyIcon(CountConnected());
	if (icon)
	{
		if (g_hTrayIcon)
		{
			DestroyIcon(g_hTrayIcon);
		}
		g_hTrayIcon = icon;
		g_nid.hIcon = g_hTrayIcon;
	}

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}

bool GetStartupStatus()
{
	auto exePath = GetModuleFsPath(g_hInst);

	wil::unique_hkey hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
	{
		wchar_t storedPath[MAX_PATH] = { 0 };
		DWORD pathLength = sizeof(storedPath);
		DWORD type = REG_SZ;
		LSTATUS result = RegQueryValueExW(hKey.get(), L"AudioPlaybackConnector", 0, &type, (LPBYTE)storedPath, &pathLength);

		if (result == ERROR_SUCCESS && type == REG_SZ && exePath == storedPath)
		{
			return true;
		}
	}

	return false;
}

void SetStartupStatus(bool status)
{
	auto exePath = GetModuleFsPath(g_hInst);

	wil::unique_hkey hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS)
	{
		if (status)
		{
			auto exePathStr = exePath.wstring();
			LOG_IF_WIN32_ERROR(RegSetValueExW(hKey.get(), L"AudioPlaybackConnector", 0, REG_SZ, (LPBYTE)exePathStr.c_str(), (lstrlenW(exePathStr.c_str()) + 1) * sizeof(wchar_t)));
		}
		else
		{
			LOG_IF_WIN32_ERROR(RegDeleteValueW(hKey.get(), L"AudioPlaybackConnector"));
		}
	}
}

void ShowInitialToastNotification()
{
	try
	{
		std::wstring title = _(L"AudioPlaybackConnector");
		std::wstring message = _(L"Application has started and is running in the notification area.");

		std::wstring toastXmlString =
			L"<toast activationType=\"protocol\" launch=\"audioplaybackconnector:show\">"
			L"<visual>"
			L"<binding template=\"ToastGeneric\">"
			L"<text>" + title + L"</text>"
			L"<text>" + message + L"</text>"
			L"</binding>"
			L"</visual>"
			L"</toast>";

		XmlDocument toastXml;
		toastXml.LoadXml(toastXmlString);

		ToastNotifier notifier{ nullptr };
		try
		{
			notifier = ToastNotificationManager::CreateToastNotifier();
		}
		catch (winrt::hresult_error const&)
		{
			LOG_CAUGHT_EXCEPTION();
			// wchar_t exePath[MAX_PATH];
			// GetModuleFileNameW(NULL, exePath, MAX_PATH);
			// std::wstring appId = exePath;
			try
			{
				// notifier = ToastNotificationManager::CreateToastNotifier(appId);
				notifier = ToastNotificationManager::CreateToastNotifier(L"AudioPlaybackConnector");
			}
			catch (winrt::hresult_error const&)
			{
				LOG_CAUGHT_EXCEPTION();
				return;
			}
		}

		// if (!notifier)
		// {
		// 	return;
		// }

		ToastNotification toast(toastXml);

		toast.ExpirationTime(winrt::Windows::Foundation::DateTime::clock::now() + std::chrono::seconds(5));

		notifier.Show(toast);
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
	catch (std::exception const&)
	{
		// Silently ignore standard exceptions from toast notification - this is not critical functionality
	}
}
