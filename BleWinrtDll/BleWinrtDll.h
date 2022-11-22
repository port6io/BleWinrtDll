#pragma once

#include "stdafx.h"

struct DeviceUpdate {
	wchar_t id[256];
	bool isConnectable = false;
	wchar_t name[256];
};

struct Service {
	wchar_t uuid[100];
};

struct Characteristic {
	wchar_t uuid[100];
	wchar_t userDescription[100];
};

struct BLEData {
	uint8_t buf[512];
	uint16_t size;
	wchar_t deviceId[256];
	wchar_t serviceUuid[256];
	wchar_t characteristicUuid[256];
};

struct ErrorMessage {
	wchar_t msg[1024];
};

enum class ScanStatus { PROCESSING, AVAILABLE, FINISHED };

extern "C" {

	__declspec(dllexport) void StartDeviceScan(wchar_t* requiredServices[], std::uint32_t n);

	__declspec(dllexport) ScanStatus PollDevice(DeviceUpdate* device, bool block);

	__declspec(dllexport) void StopDeviceScan();

	__declspec(dllexport) void ScanServices(wchar_t* deviceId);

	__declspec(dllexport) ScanStatus PollService(Service* service, bool block);

	__declspec(dllexport) void ScanCharacteristics(wchar_t* deviceId, wchar_t* serviceId);

	__declspec(dllexport) ScanStatus PollCharacteristic(Characteristic* characteristic, bool block);

	/* Return value only makes sense if block=true */
	__declspec(dllexport) bool SubscribeCharacteristic(wchar_t* deviceId, wchar_t* serviceId, wchar_t* characteristicId, bool block);

	__declspec(dllexport) bool PollData(BLEData* data, bool block);

	__declspec(dllexport) bool SendData(BLEData* data, bool block);

	__declspec(dllexport) void Disconnect(wchar_t* deviceId);

	__declspec(dllexport) void Quit();

	__declspec(dllexport) void GetError(ErrorMessage* buf);

	using DebugLogCallback = void(const char*);
	// A log callback for debugging.
	__declspec(dllexport) void RegisterLogCallback(DebugLogCallback cb);
}
