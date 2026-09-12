// Probe tool: print Bluetooth radios, AudioPlaybackConnection devices and audio
// endpoints, for researching the "reconnect after system interrupt produces no
// sound" issue. Build with build.cmd (uses the main project's generated
// cppwinrt headers).
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Media.Audio.h>

#include <cstdio>
#include <memory>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ole32.lib")

namespace wr = winrt::Windows::Devices::Radios;
namespace wde = winrt::Windows::Devices::Enumeration;

PCWSTR RadioKindName(wr::RadioKind kind)
{
	switch (kind)
	{
	case wr::RadioKind::Bluetooth: return L"Bluetooth";
	case wr::RadioKind::WiFi: return L"WiFi";
	case wr::RadioKind::MobileBroadband: return L"MBB";
	case wr::RadioKind::FM: return L"FM";
	default: return L"Other";
	}
}

PCWSTR RadioStateName(wr::RadioState state)
{
	switch (state)
	{
	case wr::RadioState::On: return L"On";
	case wr::RadioState::Off: return L"Off";
	case wr::RadioState::Disabled: return L"Disabled";
	case wr::RadioState::Unknown: return L"Unknown";
	default: return L"Other";
	}
}

void PrintEndpoints()
{
	Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), (void**)enumerator.ReleaseAndGetAddressOf());
	if (FAILED(hr))
	{
		wprintf(L"  <IMMDeviceEnumerator failed: 0x%08X>\n", hr);
		return;
	}

	Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
	hr = enumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, collection.ReleaseAndGetAddressOf());
	if (FAILED(hr))
	{
		wprintf(L"  <EnumAudioEndpoints failed: 0x%08X>\n", hr);
		return;
	}

	UINT count = 0;
	collection->GetCount(&count);
	for (UINT i = 0; i < count; ++i)
	{
		Microsoft::WRL::ComPtr<IMMDevice> device;
		if (FAILED(collection->Item(i, device.ReleaseAndGetAddressOf())))
			continue;

		LPWSTR idRaw = nullptr;
		if (FAILED(device->GetId(&idRaw)))
			continue;
		std::unique_ptr<WCHAR, decltype(&CoTaskMemFree)> id(idRaw, &CoTaskMemFree);

		DWORD state = 0;
		device->GetState(&state);
		PCWSTR stateText = L"?       ";
		switch (state)
		{
		case DEVICE_STATE_ACTIVE: stateText = L"ACTIVE   "; break;
		case DEVICE_STATE_DISABLED: stateText = L"DISABLED "; break;
		case DEVICE_STATE_NOTPRESENT: stateText = L"NOTPRESNT"; break;
		case DEVICE_STATE_UNPLUGGED: stateText = L"UNPLUGGED"; break;
		}

		Microsoft::WRL::ComPtr<IPropertyStore> props;
		if (FAILED(device->OpenPropertyStore(STGM_READ, props.ReleaseAndGetAddressOf())))
			continue;

		PROPVARIANT name{};
		props->GetValue(PKEY_Device_FriendlyName, &name);
		// Endpoint id prefix: "{0.0.0.00000000}" is render, "{0.0.1.00000000}" is capture.
		PCWSTR flowText = wcsstr(id.get(), L"{0.0.1.") != nullptr ? L"capture" : L"render ";

		wprintf(L"  [%s] %s %s\n", stateText, flowText, (name.pwszVal ? name.pwszVal : L"<no name>"));
		wprintf(L"        %s\n", id.get());
		CoTaskMemFree(name.pwszVal);
	}
}

int wmain(int argc, wchar_t** argv)
{
	winrt::init_apartment();

	// Optional: probe.exe --bt on|off  (toggle the Bluetooth radio)
	if (argc >= 3 && wcscmp(argv[1], L"--bt") == 0)
	{
		bool on = wcscmp(argv[2], L"on") == 0;
		if (!on && wcscmp(argv[2], L"off") != 0)
		{
			wprintf(L"usage: probe.exe --bt on|off
");
			return 1;
		}
		try
		{
			auto radios = wr::Radio::GetRadiosAsync().get();
			for (uint32_t i = 0; i < radios.Size(); ++i)
			{
				auto radio = radios.GetAt(i);
				if (radio.Kind() != wr::RadioKind::Bluetooth)
					continue;
				auto op = radio.SetStateAsync(on ? wr::RadioState::On : wr::RadioState::Off);
				while (op.Status() == winrt::Windows::Foundation::AsyncStatus::Started)
					Sleep(100);
				wprintf(L"bluetooth radio -> requested %s, now %s\n", on ? L"On" : L"Off", RadioStateName(radio.State()));
			}
		}
		catch (winrt::hresult_error const& e)
		{
			wprintf(L"  <radio set failed: 0x%08X %s>\n", e.code().value, e.message().c_str());
			return 1;
		}
		if (argc == 3)
			return 0;
	}

	wprintf(L"== Bluetooth radios ==\n");
	try
	{
		auto radios = wr::Radio::GetRadiosAsync().get();
		for (uint32_t i = 0; i < radios.Size(); ++i)
		{
			auto radio = radios.GetAt(i);
			auto name = radio.Name();
			wprintf(L"  %s (%s): %s\n", name.c_str(),
				RadioKindName(radio.Kind()), RadioStateName(radio.State()));
		}
	}
	catch (winrt::hresult_error const& e)
	{
		wprintf(L"  <radio enumeration failed: 0x%08X %s>\n", e.code().value, e.message().c_str());
	}

	wprintf(L"== AudioPlaybackConnection devices ==\n");
	try
	{
		auto devices = wde::DeviceInformation::FindAllAsync(
			winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector()).get();
		if (devices.Size() == 0)
			wprintf(L"  <none>\n");
		for (uint32_t i = 0; i < devices.Size(); ++i)
		{
			auto device = devices.GetAt(i);
			auto name = device.Name();
			auto id = device.Id();
			wprintf(L"  %s\n    %s\n", name.c_str(), id.c_str());
		}
	}
	catch (winrt::hresult_error const& e)
	{
		wprintf(L"  <enumeration failed: 0x%08X %s>\n", e.code().value, e.message().c_str());
	}

	wprintf(L"== Audio endpoints ==\n");
	PrintEndpoints();

	return 0;
}
