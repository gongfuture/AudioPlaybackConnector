#pragma warning(disable:4819)
#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
void ConnectDevice(const DeviceInformation& device);
winrt::fire_and_forget ConnectDeviceById(std::wstring deviceId);
winrt::fire_and_forget OpenConnectionAsync(std::wstring deviceId, uint64_t token);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();
bool GetStartupStatus();
void SetStartupStatus(bool status);
void ShowInitialToastNotification();
void SetDisplayStatusSafe(const DeviceInformation& device, std::wstring_view status, DevicePickerDisplayStatusOptions options);
std::wstring FormatConnectError(const ConnectResult& result);
size_t CountConnected();

// 把堆積上的資料交給 UI 執行緒處理。PostMessageW 是執行緒安全的，
// payload 以傳值方式接手所有權，兩條路徑的處置剛好相反：
//   失敗 -> 這則訊息不會有人收，直接讓 payload 解構把記憶體刪掉；
//   成功 -> 所有權移交給訊息佇列（WndProc 會用 unique_ptr 重新接管），
//           所以要 release() 放棄所有權，避免這裡把它刪掉造成 use-after-free。
template <typename T>
bool PostPayload(UINT message, std::unique_ptr<T> payload)
{
	if (!PostMessageW(g_hWnd, message, reinterpret_cast<WPARAM>(payload.get()), 0))
	{
		LOG_LAST_ERROR();
		return false;
	}
	payload.release();
	return true;
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
		if (item.second.connected)
		{
			++count;
		}
	}
	return count;
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

		// 先整批搬出再逐一 Close()：Close() 會同步觸發 StateChanged(Closed)，
		// 若項目還留在 map 裡，那個回呼會重入式地修改 map。
		auto connections = std::move(g_audioPlaybackConnections);
		g_audioPlaybackConnections.clear();

		for (auto& item : connections)
		{
			SetDisplayStatusSafe(item.second.device, {}, DevicePickerDisplayStatusOptions::None);
			if (item.second.connection)
			{
				item.second.connection.Close();
			}
		}

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
		/* 這個事件是由使用者觸發的斷線操作，會將對應的連線從 g_audioPlaybackConnections 中移除。 */
		auto payload = std::unique_ptr<DevicePayload>(reinterpret_cast<DevicePayload*>(wParam));
		auto it = g_audioPlaybackConnections.find(std::wstring(payload->device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			// 順序很重要：先取出 connection、再移除項目，最後才 Close()。
			// Close() 會同步觸發 StateChanged(Closed)，那個回呼會 PostMessage 過來，
			// 此時項目已經不在 map 裡，不會造成重入式的修改。
			auto connection = std::move(it->second.connection);
			g_audioPlaybackConnections.erase(it);
			if (connection)
			{
				connection.Close();
			}
		}
		SetDisplayStatusSafe(payload->device, {}, DevicePickerDisplayStatusOptions::None);
		UpdateNotifyIcon();
	}
	break;
	case WM_CONNECTRESULT:
	{
		auto result = std::unique_ptr<ConnectResult>(reinterpret_cast<ConnectResult*>(wParam));
		auto it = g_audioPlaybackConnections.find(result->deviceId);
		if (it == g_audioPlaybackConnections.end() || it->second.token != result->token)
		{
			/* 這次嘗試在進行中就被取消（使用者已斷線或又重連了），
			*  關掉這條沒人接手的連線，避免變成孤兒。 */
			if (result->connection && result->connection.State() != AudioPlaybackConnectionState::Closed)
			{
				result->connection.Close();
			}
			break;
		}

		if (result->success)
		{
			it->second.connection = result->connection;
			it->second.connected = true;
			SetDisplayStatusSafe(it->second.device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
		}
		else
		{
			auto device = it->second.device;
			g_audioPlaybackConnections.erase(it);
			if (result->connection && result->connection.State() != AudioPlaybackConnectionState::Closed)
			{
				result->connection.Close();
			}
			SetDisplayStatusSafe(device, FormatConnectError(*result), DevicePickerDisplayStatusOptions::ShowRetryButton);
		}
		UpdateNotifyIcon();
	}
	break;
	case WM_CONNECTIONCLOSED:
	{
		/* 再找一次是否有對應的連線目的在於防止*非使用者*觸發的連線關閉事件 (如藍芽距離太遠斷線等)，此時只會有WM_CONNECTIONCLOSED事件發生；
		*  如果是使用者觸發的斷線事件，會先觸發WM_DISCONNECTDEVICE事件，並將對應的連線從g_audioPlaybackConnections移除，這時WM_CONNECTIONCLOSED事件就不會再做任何事。 */
		auto payload = std::unique_ptr<ClosedPayload>(reinterpret_cast<ClosedPayload*>(wParam));
		auto it = g_audioPlaybackConnections.find(payload->deviceId);
		if (it != g_audioPlaybackConnections.end() && it->second.token == payload->token)
		{
			auto device = it->second.device;
			g_audioPlaybackConnections.erase(it);
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
		// 已經連上或正在連線中，只要把狀態補回 picker 就好。
		SetDisplayStatusSafe(it->second.device,
			it->second.connected ? _(L"Connected") : _(L"Connecting"),
			it->second.connected
			? DevicePickerDisplayStatusOptions::ShowDisconnectButton
			: (DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton));
		return;
	}

	const auto token = g_nextConnectToken++;
	it->second.device = device;
	it->second.token = token;
	it->second.connected = false;

	SetDisplayStatusSafe(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	OpenConnectionAsync(std::move(deviceId), token);
}

// 整個連線流程都在背景執行緒上跑，結果一律用 WM_CONNECTRESULT 交回 UI 執行緒。
// 這裡不可以碰 g_audioPlaybackConnections，也不可以呼叫 _()：Translate() 內部的
// 快取是非執行緒安全的 static map。
winrt::fire_and_forget OpenConnectionAsync(std::wstring deviceId, uint64_t token)
{
	co_await winrt::resume_background();

	auto result = std::make_unique<ConnectResult>();
	result->deviceId = deviceId;
	result->token = token;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(deviceId);
		if (!connection)
		{
			result->created = false;
		}
		else
		{
			result->connection = connection;

			connection.StateChanged([token](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					auto payload = std::make_unique<ClosedPayload>();
					payload->deviceId = sender.DeviceId();
					payload->token = token;
					PostPayload(WM_CONNECTIONCLOSED, std::move(payload));
				}
				});

			co_await connection.StartAsync();
			auto openResult = co_await connection.OpenAsync();

			result->status = openResult.Status();
			result->success = result->status == AudioPlaybackConnectionOpenResultStatus::Success;
			if (result->status == AudioPlaybackConnectionOpenResultStatus::UnknownFailure)
			{
				result->error = openResult.ExtendedError();
			}
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		result->success = false;
		result->status = AudioPlaybackConnectionOpenResultStatus::UnknownFailure;
		result->error = ex.code();
		LOG_CAUGHT_EXCEPTION();
	}

	PostPayload(WM_CONNECTRESULT, std::move(result));
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

// 只在 UI 執行緒呼叫。
std::wstring FormatConnectError(const ConnectResult& result)
{
	if (!result.created)
	{
		return _(L"Unknown error");
	}

	switch (result.status)
	{
	case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
		return _(L"The request timed out");
	case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
		return _(L"The operation was denied by the system");
	default:
		break;
	}

	if (result.error != S_OK)
	{
		auto message = winrt::hresult_error(result.error).message();
		std::wstring formatted(64, L'\0');
		while (1)
		{
			auto written = swprintf(formatted.data(), formatted.size(), L"%s (0x%08X)", message.c_str(), static_cast<uint32_t>(result.error));
			if (written < 0)
			{
				formatted.resize(formatted.size() * 2);
			}
			else
			{
				formatted.resize(written);
				break;
			}
		}
		return formatted;
	}

	return _(L"Unknown error");
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
