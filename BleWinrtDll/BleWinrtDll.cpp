// BleWinrtDll.cpp : Definiert die exportierten Funktionen für die DLL-Anwendung.
//

#include "stdafx.h"
#include <future>

#include "BleWinrtDll.h"

#pragma comment(lib, "windowsapp")

// macro for file, see also https://stackoverflow.com/a/14421702
#define __WFILE__ L"BleWinrtDll.cpp"

using namespace std;

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Web::Syndication;

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Enumeration;

using namespace winrt::Windows::Storage::Streams;

DebugLogCallback* logger = nullptr;
void Log(const char* s) {
	if (logger)
		(*logger)(s);
}

string convert_to_string(const wstring& wstr)
{
	// https://stackoverflow.com/questions/215963/how-do-you-properly-use-widechartomultibyte
	if (wstr.empty()) return string();
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
	string strTo(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
	return strTo;
}

void Log(const wstring& s) {
	if (logger)
		(*logger)(convert_to_string(s).c_str());
}

union to_guid
{
	uint8_t buf[16];
	winrt::guid guid;
};

const uint8_t BYTE_ORDER[] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };

winrt::guid make_guid(const wchar_t* value)
{
	to_guid to_guid;
	memset(&to_guid, 0, sizeof(to_guid));
	int offset = 0;
	for (int i = 0; i < (int)wcslen(value); i++) {
		if (value[i] >= '0' && value[i] <= '9')
		{
			uint8_t digit = value[i] - '0';
			to_guid.buf[BYTE_ORDER[offset / 2]] += offset % 2 == 0 ? digit << 4 : digit;
			offset++;
		}
		else if (value[i] >= 'A' && value[i] <= 'F')
		{
			uint8_t digit = 10 + value[i] - 'A';
			to_guid.buf[BYTE_ORDER[offset / 2]] += offset % 2 == 0 ? digit << 4 : digit;
			offset++;
		}
		else if (value[i] >= 'a' && value[i] <= 'f')
		{
			uint8_t digit = 10 + value[i] - 'a';
			to_guid.buf[BYTE_ORDER[offset / 2]] += offset % 2 == 0 ? digit << 4 : digit;
			offset++;
		}
		else
		{
			// skip char
		}
	}

	return to_guid.guid;
}

// implement own caching instead of using the system-provicded cache as there is an AccessDenied error when trying to
// call GetCharacteristicsAsync on a service for which a reference is hold in global scope
// cf. https://stackoverflow.com/a/36106137

// Perhaps more relevant?
// https://stackoverflow.com/questions/71620883/ble-using-winrt-access-denied-when-executing-getcharacteristicsforuuidasync

mutex errorLock;
wchar_t last_error[2048];
struct CharacteristicCacheEntry {
	GattCharacteristic characteristic = nullptr;
};
struct ServiceCacheEntry {
	GattDeviceService service = nullptr;
	map<long, CharacteristicCacheEntry> characteristics = { };
};
struct DeviceCacheEntry {
	BluetoothLEDevice device = nullptr;
	map<long, ServiceCacheEntry> services = { };
};
// Seems like a very necessary lock... but could not get it working :(
mutex cacheLock;
map<long, DeviceCacheEntry> cache;


// using hashes of uuids to omit storing the c-strings in reliable storage
long hsh(const wchar_t* wstr)
{
	long hash = 5381;
	int c;
	while (c = *wstr++)
		hash = ((hash << 5) + hash) + c;
	return hash;
}

void clearError() {
	lock_guard error_lock(errorLock);
	wcscpy_s(last_error, L"Ok");
}

void saveError(const wchar_t* message, ...) {
	lock_guard error_lock(errorLock);
	va_list args;
	va_start(args, message);
	vswprintf_s(last_error, message, args);
	va_end(args);
	wcout << last_error << endl;
}

IAsyncOperation<BluetoothLEDevice> retrieveDevice(const wchar_t* deviceId) {
	{
		lock_guard lock(cacheLock);
		auto item = cache.find(hsh(deviceId));
		if (item != cache.end())
		{
			Log(L"Using cached connection");
			co_return item->second.device;
		}
	}
	// !!!! BluetoothLEDevice.FromIdAsync may prompt for consent, in this case bluetooth will fail in unity!
	BluetoothLEDevice result = co_await BluetoothLEDevice::FromIdAsync(deviceId);
	if (result == nullptr) {
		saveError(L"%s:%d Failed to connect to device.", __WFILE__, __LINE__);
		co_return nullptr;
	}
	else {
		try {
			clearError();
			co_await chrono::seconds(1);
			{
				lock_guard lock(cacheLock);
				cache[hsh(deviceId)] = { result };
				const auto& device = cache[hsh(deviceId)].device;
				Log(L"Connected " + wstring(deviceId));
				co_return device;
			}
		}
		catch (const std::exception&) {
			saveError(L"Connection to %s failed", __WFILE__, __LINE__, deviceId);
		}
	}
}
IAsyncOperation<GattDeviceService> retrieveService(const wchar_t* deviceId, const wchar_t* serviceId) {
	auto device = co_await retrieveDevice(deviceId);
	if (device == nullptr)
		co_return nullptr;
	{
		lock_guard lock(cacheLock);
		if (cache[hsh(deviceId)].services.count(hsh(serviceId)))
		{
			Log("Using cached service");
			co_return cache[hsh(deviceId)].services[hsh(serviceId)].service;
		}
	}
	GattDeviceServicesResult result = co_await device.GetGattServicesForUuidAsync(make_guid(serviceId), BluetoothCacheMode::Uncached);
	if (result.Status() != GattCommunicationStatus::Success) {
		saveError(L"%s:%d Failed retrieving services.", __WFILE__, __LINE__);
		co_return nullptr;
	}
	else if (result.Services().Size() == 0) {
		saveError(L"%s:%d No service found with uuid ", __WFILE__, __LINE__);
		co_return nullptr;
	}
	else {
		clearError();
		{
			lock_guard lock(cacheLock);
			cache[hsh(deviceId)].services[hsh(serviceId)] = { result.Services().GetAt(0) };
			co_return cache[hsh(deviceId)].services[hsh(serviceId)].service;
		}
	}
}
IAsyncOperation<GattCharacteristic> retrieveCharacteristic(const wchar_t* deviceId, const wchar_t* serviceId, const wchar_t* characteristicId) {
	Log("retrieveCharacteristic");
	auto service = co_await retrieveService(deviceId, serviceId);
	if (service == nullptr)
	{
		Log("Service retrieve failed");
		co_return nullptr;
	}

	{
		lock_guard lock(cacheLock);
		if (cache[hsh(deviceId)].services[hsh(serviceId)].characteristics.count(hsh(characteristicId)))
		{
			Log("Cached characteristic");
			co_return cache[hsh(deviceId)].services[hsh(serviceId)].characteristics[hsh(characteristicId)].characteristic;
		}
	}
	GattCharacteristicsResult result = co_await service.GetCharacteristicsForUuidAsync(make_guid(characteristicId), BluetoothCacheMode::Cached);
	if (result.Status() != GattCommunicationStatus::Success) {
		saveError(L"%s:%d Error in getCharacteristicsForUuid from service %s and characteristic %s with status %d",
			__WFILE__, __LINE__, serviceId, characteristicId, result.Status());
		co_return nullptr;
	}
	else if (result.Characteristics().Size() == 0) {
		saveError(L"%s:%d No characteristic found with uuid %s for service %s", __WFILE__, __LINE__, characteristicId, serviceId);
		co_return nullptr;
	}
	else {
		clearError();
		{
			lock_guard lock(cacheLock);
			cache[hsh(deviceId)].services[hsh(serviceId)].characteristics[hsh(characteristicId)] = { result.Characteristics().GetAt(0) };
			co_return cache[hsh(deviceId)].services[hsh(serviceId)].characteristics[hsh(characteristicId)].characteristic;
		}
	}
}


BluetoothLEAdvertisementWatcher deviceWatcher{ nullptr };
BluetoothLEAdvertisementWatcher::Received_revoker deviceWatcherReceivedRevoker;

queue<DeviceUpdate> deviceQueue{};
mutex deviceQueueLock;
condition_variable deviceQueueSignal;
bool deviceScanFinished;

queue<Service> serviceQueue{};
mutex serviceQueueLock;
condition_variable serviceQueueSignal;
bool serviceScanFinished;

queue<Characteristic> characteristicQueue{};
mutex characteristicQueueLock;
condition_variable characteristicQueueSignal;
bool characteristicScanFinished;

// global flag to release calling thread
mutex quitLock;
atomic<bool> quitFlag = false;

struct Subscription {
	GattCharacteristic characteristic = nullptr;
	GattCharacteristic::ValueChanged_revoker revoker;
	template <class A, class B>
	Subscription(A&& a, B&& b) : characteristic(std::forward<A>(a)), revoker(std::forward<B>(b)) { }
};
list<Subscription> subscriptions;
mutex subscribeQueueLock;
condition_variable subscribeQueueSignal;

queue<BLEData> dataQueue{};
mutex dataQueueLock;
condition_variable dataQueueSignal;

bool QuittableWait(condition_variable& signal, unique_lock<mutex>& waitLock) {
	{
		if (quitFlag)
			return true;
	}
	signal.wait(waitLock);
	return quitFlag;
}

winrt::fire_and_forget DeviceWatcher_Received(BluetoothLEAdvertisementWatcher watcher, BluetoothLEAdvertisementReceivedEventArgs eventArgs) {
	auto dev = co_await BluetoothLEDevice::FromBluetoothAddressAsync(eventArgs.BluetoothAddress());
	DeviceUpdate deviceUpdate;
	deviceUpdate.isConnectable = dev.DeviceInformation().Pairing().CanPair();
	const auto nameLen = sizeof(deviceUpdate.name) / sizeof(wchar_t);
	Log(L"Scan received " + wstring(eventArgs.Advertisement().LocalName().c_str()));
	// TODO: Parametrize manufacturer id
	auto manData = eventArgs.Advertisement().GetManufacturerDataByCompanyId(0xFFFF);
	if (manData.begin() != manData.end()) 
	{
		const auto& data = manData.First().Current().Data();
		const auto n = min(data.Length(), sizeof(deviceUpdate.advData) / sizeof(deviceUpdate.advData[0]));
		memcpy(deviceUpdate.advData, data.data(), n * sizeof(deviceUpdate.advData[0]));
		deviceUpdate.advDataLen = (decltype(deviceUpdate.advDataLen)) n;
		Log(L"Scan manufacturer data has length " + std::to_wstring((int)n));
	}
	wcscpy_s(deviceUpdate.id, sizeof(deviceUpdate.id) / sizeof(wchar_t), dev.DeviceInformation().Id().c_str());
	wcscpy_s(deviceUpdate.name, nameLen, eventArgs.Advertisement().LocalName().c_str());
	{
		if (quitFlag)
			co_return;
	}
	{
		lock_guard queueGuard(deviceQueueLock);
		deviceQueue.push(deviceUpdate);
		deviceQueueSignal.notify_one();
	}
}

void StartDeviceScan(wchar_t* requiredServices[], std::uint32_t n) {
	// as this is the first function that must be called, if Quit() was called before, assume here that the client wants to restart
	quitFlag = false;
	clearError();
	Log("StartDeviceScan");
	{
		lock_guard lock(deviceQueueLock);
		while (deviceQueue.size() > 0)
			deviceQueue.pop();
	}

	deviceWatcher = BluetoothLEAdvertisementWatcher();
	deviceWatcher.AllowExtendedAdvertisements(true);
	deviceWatcher.ScanningMode(BluetoothLEScanningMode::Active);

	for (std::uint32_t i = 0; i < n; i++) {
		deviceWatcher.AdvertisementFilter().Advertisement().ServiceUuids().Append(make_guid(requiredServices[i]));
	}
	deviceWatcherReceivedRevoker = deviceWatcher.Received(winrt::auto_revoke, &DeviceWatcher_Received);
	deviceWatcher.Start();
	// deviceQueueSignal.notify_one();
}

void StopDeviceScan() {
	Log("StopDeviceScan");
	lock_guard lock(deviceQueueLock);
	if (deviceWatcher != nullptr) {
		deviceWatcherReceivedRevoker.revoke();
		deviceWatcher.Stop();
		deviceWatcher = nullptr;
	}
	deviceScanFinished = true;
	deviceQueueSignal.notify_one();
}

ScanStatus PollDevice(DeviceUpdate* device, bool block) {
	ScanStatus res;
	unique_lock<mutex> lock(deviceQueueLock);
	if (block && deviceQueue.empty() && !deviceScanFinished)
		if (QuittableWait(deviceQueueSignal, lock))
			return ScanStatus::FINISHED;
	if (!deviceQueue.empty()) {
		*device = deviceQueue.front();
		deviceQueue.pop();
		res = ScanStatus::AVAILABLE;
	}
	else if (deviceScanFinished)
		res = ScanStatus::FINISHED;
	else
		res = ScanStatus::PROCESSING;
	return res;
}

winrt::fire_and_forget ScanServicesAsync(wchar_t* deviceId) {
	{
		lock_guard queueGuard(serviceQueueLock);
		serviceScanFinished = false;
	}
	Log(L"Schanning services of " + wstring(deviceId));
	try {
		const auto bluetoothLeDevice = co_await retrieveDevice(deviceId);
		co_await chrono::milliseconds(1); // Never too much waiting with ble
		if (bluetoothLeDevice != nullptr) {
			Log("GetGattServicesAsync");
			GattDeviceServicesResult result = co_await bluetoothLeDevice.GetGattServicesAsync(BluetoothCacheMode::Uncached);
			if (result.Status() == GattCommunicationStatus::Success) {
				Log("GetGattServicesAsync succeeded");
				IVectorView<GattDeviceService> services = result.Services();
				for (const auto& service : services)
				{
					Service serviceStruct;
					wcscpy_s(serviceStruct.uuid, sizeof(serviceStruct.uuid) / sizeof(wchar_t), to_hstring(service.Uuid()).c_str());
					{
						if (quitFlag)
							break;
					}
					{
						lock_guard queueGuard(serviceQueueLock);
						serviceQueue.push(serviceStruct);
						serviceQueueSignal.notify_one();
					}
					// {
					// 	Log(L"Caching service " + wstring(serviceStruct.uuid));
					// 	lock_guard lock(cacheLock);
					// 	cache[hsh(deviceId)].services[hsh(serviceStruct.uuid)] = { service };
					// }
				}
			}
			else {
				saveError(L"%s:%d Failed retrieving services.", __WFILE__, __LINE__);
			}
		}
	}
	catch (winrt::hresult_error& ex)
	{
		saveError(L"%s:%d ScanServicesAsync catch: %s", __WFILE__, __LINE__, ex.message().c_str());
	}
	{
		lock_guard queueGuard(serviceQueueLock);
		serviceScanFinished = true;
		serviceQueueSignal.notify_one();
	}
}
void ScanServices(wchar_t* deviceId) {
	ScanServicesAsync(deviceId);
}

ScanStatus PollService(Service* service, bool block) {
	ScanStatus res;
	unique_lock<mutex> lock(serviceQueueLock);
	if (block && serviceQueue.empty() && !serviceScanFinished)
		if (QuittableWait(serviceQueueSignal, lock))
			return ScanStatus::FINISHED;
	if (!serviceQueue.empty()) {
		*service = serviceQueue.front();
		serviceQueue.pop();
		res = ScanStatus::AVAILABLE;
	}
	else if (serviceScanFinished)
		res = ScanStatus::FINISHED;
	else
		res = ScanStatus::PROCESSING;
	return res;
}

winrt::fire_and_forget ScanCharacteristicsAsync(unique_ptr<wstring> deviceId, unique_ptr<wstring> serviceId) {
	Log(L"Scanning characteristics of " + *deviceId);
	{
		lock_guard lock(characteristicQueueLock);
		characteristicScanFinished = false;
	}
	try {
		auto service = co_await retrieveService(deviceId->c_str(), serviceId->c_str());
		co_await chrono::milliseconds(100);
		if (service != nullptr) {
			GattCharacteristicsResult charScan = co_await service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached);
			if (charScan.Status() != GattCommunicationStatus::Success)
				saveError(L"%s:%d Error scanning characteristics from service %s width status %d", __WFILE__, __LINE__, *serviceId, (int)charScan.Status());
			else {
				for (auto c : charScan.Characteristics())
				{
					Characteristic charStruct;
					wcscpy_s(charStruct.uuid, sizeof(charStruct.uuid) / sizeof(wchar_t), to_hstring(c.Uuid()).c_str());
					// retrieve user description
					GattDescriptorsResult descriptorScan = co_await c.GetDescriptorsForUuidAsync(make_guid(L"00002901-0000-1000-8000-00805F9B34FB"), BluetoothCacheMode::Uncached);
					if (descriptorScan.Descriptors().Size() == 0) {
						const wchar_t* defaultDescription = L"no description available";
						wcscpy_s(charStruct.userDescription, sizeof(charStruct.userDescription) / sizeof(wchar_t), defaultDescription);
					}
					else {
						GattDescriptor descriptor = descriptorScan.Descriptors().GetAt(0);
						auto nameResult = co_await descriptor.ReadValueAsync();
						if (nameResult.Status() != GattCommunicationStatus::Success)
							saveError(L"%s:%d couldn't read user description for charasteristic %s, status %d", __WFILE__, __LINE__, to_hstring(c.Uuid()).c_str(), nameResult.Status());
						else {
							auto dataReader = DataReader::FromBuffer(nameResult.Value());
							auto output = dataReader.ReadString(dataReader.UnconsumedBufferLength());
							wcscpy_s(charStruct.userDescription, sizeof(charStruct.userDescription) / sizeof(wchar_t), output.c_str());
							clearError();
						}
					}
					if (quitFlag)
						break;
					// {
					// 	const auto hash = hsh(charStruct.uuid);
					// 	Log(L"Caching " + wstring(charStruct.uuid) + L" of service " + *serviceId + L" and hash " + to_wstring(hash));
					// 	lock_guard lock(cacheLock);
					// 	cache[hsh(deviceId->c_str())].services[hsh(serviceId->c_str())].characteristics[hash] = { c };
					// }
					{
						lock_guard queueGuard(characteristicQueueLock);
						characteristicQueue.push(charStruct);
						characteristicQueueSignal.notify_one();
					}
				}
			}
		}
	}
	catch (winrt::hresult_error& ex)
	{
		saveError(L"%s:%d ScanCharacteristicsAsync catch: %s", __WFILE__, __LINE__, ex.message().c_str());
	}
	{
		lock_guard lock(characteristicQueueLock);
		characteristicScanFinished = true;
		characteristicQueueSignal.notify_one();
	}
}

void ScanCharacteristics(wchar_t* deviceId, wchar_t* serviceId) {
	ScanCharacteristicsAsync(make_unique<wstring>(deviceId), make_unique<wstring>(serviceId));
}

ScanStatus PollCharacteristic(Characteristic* characteristic, bool block) {
	ScanStatus res;
	unique_lock<mutex> lock(characteristicQueueLock);
	if (block && characteristicQueue.empty() && !characteristicScanFinished)
		if (QuittableWait(characteristicQueueSignal, lock))
			return ScanStatus::FINISHED;
	if (!characteristicQueue.empty()) {
		*characteristic = characteristicQueue.front();
		characteristicQueue.pop();
		res = ScanStatus::AVAILABLE;
	}
	else if (characteristicScanFinished)
		res = ScanStatus::FINISHED;
	else
		res = ScanStatus::PROCESSING;
	return res;
}

void Characteristic_ValueChanged(GattCharacteristic const& characteristic, GattValueChangedEventArgs args)
{
	// Log(L"Characteristic_ValueChanged " + wstring(to_hstring(characteristic.Uuid()).c_str()));
	BLEData data;
	wcscpy_s(data.characteristicUuid, sizeof(data.characteristicUuid) / sizeof(wchar_t), to_hstring(characteristic.Uuid()).c_str());
	wcscpy_s(data.serviceUuid, sizeof(data.serviceUuid) / sizeof(wchar_t), to_hstring(characteristic.Service().Uuid()).c_str());
	wcscpy_s(data.deviceId, sizeof(data.deviceId) / sizeof(wchar_t), characteristic.Service().Device().DeviceId().c_str());

	data.size = args.CharacteristicValue().Length();
	// IBuffer to array, copied from https://stackoverflow.com/a/55974934
	memcpy(data.buf, args.CharacteristicValue().data(), data.size);

	{
		if (quitFlag)
			return;
	}
	{
		lock_guard queueGuard(dataQueueLock);
		dataQueue.push(data);
		dataQueueSignal.notify_one();
	}
}

winrt::fire_and_forget SubscribeCharacteristicAsync(unique_ptr<wstring> deviceId, unique_ptr<wstring> serviceId, unique_ptr<wstring> characteristicId, bool* result) {
	Log("SubscribeCharacteristicAsync");
	try {
		auto characteristic = co_await retrieveCharacteristic(deviceId->c_str(), serviceId->c_str(), characteristicId->c_str());
		if (characteristic != nullptr) {
			auto status = co_await characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue::Notify);
			if (status != GattCommunicationStatus::Success)
				saveError(L"%s:%d Error subscribing to characteristic with uuid %s and status %d", __WFILE__, __LINE__, *characteristicId, status);
			else {
				Log("Subscription successful");
				subscriptions.emplace_back(characteristic, characteristic.ValueChanged(winrt::auto_revoke, &Characteristic_ValueChanged));
				if (result != nullptr)
					*result = true;
			}
		}
	}
	catch (winrt::hresult_error& ex)
	{
		saveError(L"%s:%d SubscribeCharacteristicAsync catch: %s", __WFILE__, __LINE__, ex.message().c_str());
	}
	subscribeQueueSignal.notify_one();
}
/* */
bool SubscribeCharacteristic(wchar_t* deviceId, wchar_t* serviceId, wchar_t* characteristicId, bool block) {
	Log(L"SubsctribeCharacteristic " + wstring(characteristicId));
	unique_lock<mutex> lock(subscribeQueueLock);
	bool result = false;
	SubscribeCharacteristicAsync(make_unique<wstring>(deviceId), make_unique<wstring>(serviceId), make_unique<wstring>(characteristicId), block ? &result : 0);
	if (block && QuittableWait(subscribeQueueSignal, lock))
		return false;

	return result;
}

bool PollData(BLEData* data, bool block) {
	unique_lock<mutex> lock(dataQueueLock);
	if (block && dataQueue.empty())
		if (QuittableWait(dataQueueSignal, lock))
			return false;
	if (!dataQueue.empty()) {
		*data = dataQueue.front();
		dataQueue.pop();
		return true;
	}
	return false;
}

winrt::fire_and_forget SendDataAsync(unique_ptr<BLEData> data, condition_variable* signal, bool* result) {
	try {
		auto characteristic = co_await retrieveCharacteristic(data->deviceId, data->serviceUuid, data->characteristicUuid);
		if (characteristic != nullptr) {
			// create IBuffer from data
			DataWriter writer;
			writer.WriteBytes(winrt::array_view<uint8_t const>(data->buf, data->buf + data->size));
			IBuffer buffer = writer.DetachBuffer();
			auto status = co_await characteristic.WriteValueAsync(buffer, GattWriteOption::WriteWithoutResponse);
			if (status != GattCommunicationStatus::Success)
				saveError(L"%s:%d Error writing value to characteristic with uuid %s", __WFILE__, __LINE__, data->characteristicUuid);
			else if (result != 0)
				*result = true;
		}
	}
	catch (winrt::hresult_error& ex)
	{
		saveError(L"%s:%d SendDataAsync catch: %s", __WFILE__, __LINE__, ex.message().c_str());
	}
	if (signal != 0)
		signal->notify_one();
}
bool SendData(BLEData* data, bool block) {
	mutex _mutex;
	unique_lock<mutex> lock(_mutex);
	condition_variable signal;
	bool result = false;
	// copy data to stack so that caller can free its memory in non-blocking mode
	SendDataAsync(make_unique<BLEData>(*data), block ? &signal : 0, block ? &result : 0);
	if (block)
		signal.wait(lock);

	return result;
}

void Disconnect(wchar_t* deviceId)
{
	try {
		std::wstring msg = L"BleWinRT Disconnect...";
		msg += deviceId;
		Log(msg);
		const auto hash = hsh(deviceId);
		{
			lock_guard lock(cacheLock);
			const auto devP = cache.find(hash);
			if (devP == cache.end())
				return;
			Log("Cache entry found");
			// Copying device and services as in Quit function. 
			auto dev = devP->second;
			if (dev.device != nullptr)
				dev.device.Close();
			Log("Device closed. Services...");
			for (auto service : dev.services)
			{
				msg = L"Closing service " + std::to_wstring(service.first);
				Log(msg);
				service.second.service.Close();
			}
			cache.erase(hash);
		}

	}
	catch (const std::exception& e) {
		Log(e.what());
	}
}

void Quit() {
	quitFlag = true;
	StopDeviceScan();
	deviceQueueSignal.notify_one();
	{
		lock_guard lock(deviceQueueLock);
		deviceQueue = {};
	}
	serviceQueueSignal.notify_one();
	{
		lock_guard lock(serviceQueueLock);
		serviceQueue = {};
	}
	characteristicQueueSignal.notify_one();
	{
		lock_guard lock(characteristicQueueLock);
		characteristicQueue = {};
	}
	subscribeQueueSignal.notify_one();
	{
		lock_guard lock(subscribeQueueLock);
		for (auto& subscription : subscriptions)
			subscription.revoker.revoke();
		subscriptions.clear();
	}
	dataQueueSignal.notify_one();
	{
		lock_guard lock(dataQueueLock);
		dataQueue = {};
	}
	{
		lock_guard lock(cacheLock);
		/* Copy as per original source. Dunno why. */
		for (auto device : cache) {
			device.second.device.Close();
			for (auto service : device.second.services) {
				service.second.service.Close();
			}
		}
		cache.clear();
	}
}

void GetError(ErrorMessage* buf) {
	lock_guard error_lock(errorLock);
	wcscpy_s(buf->msg, last_error);
}

const wchar_t* formatBluetoothAddress(unsigned long long BluetoothAddress) {
	std::wostringstream ret;
	ret << std::hex << std::setfill(L'0')
		<< std::setw(2) << ((BluetoothAddress >> (5 * 8)) & 0xff) << ":"
		<< std::setw(2) << ((BluetoothAddress >> (4 * 8)) & 0xff) << ":"
		<< std::setw(2) << ((BluetoothAddress >> (3 * 8)) & 0xff) << ":"
		<< std::setw(2) << ((BluetoothAddress >> (2 * 8)) & 0xff) << ":"
		<< std::setw(2) << ((BluetoothAddress >> (1 * 8)) & 0xff) << ":"
		<< std::setw(2) << ((BluetoothAddress >> (0 * 8)) & 0xff);
	return ret.str().c_str();
}

void RegisterLogCallback(DebugLogCallback cb) {
	logger = cb;
}