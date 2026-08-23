#pragma once

// Internal helper shared by the BlueZ backend translation units. Not part of the module's
// public interface -- it deliberately lives under src/ so no openDAQ-facing header ever needs
// to include systemd or D-Bus headers.

#include <bluetooth_module/ble/ble_types.h>

#include <systemd/sd-bus.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opendaq_bt::ble::bluez
{

/*! The subset of D-Bus variant payloads BlueZ uses for the properties we consume. */
struct Value
{
    char type = 0;
    std::string str;
    int64_t integer = 0;
    bool boolean = false;
    std::vector<std::string> strings;
    std::vector<uint8_t> bytes;
    std::map<uint16_t, std::vector<uint8_t>> byteArraysByKey;
};

using Properties = std::map<std::string, Value>;

/*! object path -> interface name -> properties, i.e. one GetManagedObjects reply. */
using ManagedObjects = std::map<std::string, std::map<std::string, Properties>>;

/*!
 * Owns one system-bus connection plus the thread that drives it.
 *
 * sd-bus connections are not safe for concurrent use, so every call into the bus is serialised
 * on @c mutex. The dispatch thread polls the bus file descriptor *outside* that lock and only
 * takes it to run sd_bus_process, which keeps a slow blocking call (Connect can take seconds)
 * from starving signal delivery for longer than the call itself.
 */
class Bus
{
public:
    Bus();
    ~Bus();

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    bool open(std::string& error);
    void close();
    bool isOpen() const { return bus != nullptr; }

    /*! Serialises access for callers that need to issue several calls atomically. */
    std::mutex& lock() { return mutex; }

    sd_bus* raw() { return bus; }

    /*! Calls a method with no arguments and discards the reply. Returns false and fills @p error. */
    bool callVoid(const std::string& path,
                  const std::string& iface,
                  const std::string& member,
                  std::string& error,
                  uint64_t timeoutUsec = 0);

    /*! org.freedesktop.DBus.ObjectManager.GetManagedObjects on "/". */
    bool getManagedObjects(ManagedObjects& objects, std::string& error);

    /*! Reads a single property through org.freedesktop.DBus.Properties.Get. */
    bool getProperty(const std::string& path,
                     const std::string& iface,
                     const std::string& name,
                     Value& value,
                     std::string& error);

    /*! Installs a signal match; the callback runs on the dispatch thread. */
    bool matchSignal(const std::string& path,
                     const std::string& iface,
                     const std::string& member,
                     sd_bus_message_handler_t handler,
                     void* userdata,
                     std::string& error);

    void startDispatch();
    void stopDispatch();

private:
    void dispatchLoop();
    void wake();

    sd_bus* bus = nullptr;
    std::mutex mutex;
    std::vector<sd_bus_slot*> slots;

    std::thread dispatcher;
    volatile bool running = false;
    int wakeFd = -1;
};

/*! Reads a D-Bus variant (the message must be positioned at the variant) into @p value. */
int readVariant(sd_bus_message* m, Value& value);

/*! Reads an a{sv} property dictionary. */
int readProperties(sd_bus_message* m, Properties& properties);

/*! "AA:BB:CC:DD:EE:FF" -> "dev_AA_BB_CC_DD_EE_FF". */
std::string addressToNodeName(const std::string& address);

/*! Maps BlueZ GattCharacteristic1 "Flags" strings onto our property bitmask. */
uint32_t flagsToProperties(const std::vector<std::string>& flags);

}
