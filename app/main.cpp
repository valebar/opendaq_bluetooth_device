// bt_cli -- a small harness for exercising the Bluetooth module against real hardware.
//
//   bt_cli scan [seconds]        list nearby BLE peripherals
//   bt_cli connect <conn-string> connect and print the GATT database
//   bt_cli read <conn-string> <characteristic>
//   bt_cli write <conn-string> <characteristic> <value>
//   bt_cli monitor <conn-string> [seconds] [channel]
//
// It drives the module the same way any openDAQ application would: through an Instance whose
// ModuleManager loads the built module from disk.

#include <opendaq/opendaq.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace daq;

namespace
{

InstancePtr createInstance()
{
    // Prefer the directory this executable lives in, which is where the module is built.
    //
    // Config providers are opt-in: an InstanceBuilder runs none by default, and even
    // enableStandardProviders() adds only the JSON one. Adding the environment provider here is
    // what makes OPENDAQ_CONFIG_Modules_BluetoothModule_FilterByKnownProfiles=false work.
    return InstanceBuilder()
        .addModulePath(OPENDAQ_BT_MODULE_DIR)
        .addConfigProvider(EnvConfigProvider())
        .build();
}

std::string propertyOr(const PropertyObjectPtr& object, const std::string& name, const std::string& fallback)
{
    if (!object.assigned() || !object.hasProperty(name))
        return fallback;

    const auto value = object.getPropertyValue(name);
    return value.assigned() ? static_cast<std::string>(value.toString()) : fallback;
}

bool boolPropertyOr(const PropertyObjectPtr& object, const std::string& name, bool fallback)
{
    // Deliberately not propertyOr: that renders through toString(), which turns a Bool into the
    // strings "True"/"False" rather than something to branch on.
    if (!object.assigned() || !object.hasProperty(name))
        return fallback;

    const auto value = object.getPropertyValue(name);
    return value.assigned() ? static_cast<bool>(static_cast<Bool>(value)) : fallback;
}

constexpr char FilterEnvVariable[] = "OPENDAQ_CONFIG_Modules_BluetoothModule_FilterByKnownProfiles";

bool filterDisabledByEnvironment()
{
    const char* value = std::getenv(FilterEnvVariable);
    if (value == nullptr)
        return false;

    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return text == "false";
}

int scan(int seconds)
{
    const auto instance = createInstance();

    std::cout << "Scanning for " << seconds << "s...\n";

    ListPtr<IDeviceInfo> devices;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    do
    {
        devices = instance.getAvailableDevices();
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (true);

    // Addresses are not a fixed width across platforms: a MAC is 17 characters, but a macOS
    // CoreBluetooth peripheral UUID is 36, which runs straight into a column sized for the
    // former. Measure the rows first and size the column to the widest one actually present.
    std::vector<std::array<std::string, 3>> rows;
    int connectedCount = 0;
    for (const auto& info : devices)
    {
        const std::string connectionString = info.getConnectionString();
        if (connectionString.rfind("bluetooth://", 0) != 0)
            continue;

        // A peripheral connected to this host is not advertising, so there is no reading to show.
        // Printing the 0 the module reports would rank it as the strongest signal on the desk.
        const bool connected = boolPropertyOr(info, "Connected", false);
        if (connected)
            ++connectedCount;

        std::string rssi = propertyOr(info, "Rssi", "?");
        if (connected && rssi == "0")
            rssi = "-";

        rows.push_back({propertyOr(info, "Address", "?"),
                        std::move(rssi),
                        static_cast<std::string>(info.getName())});
    }

    std::streamsize addressWidth = 22;
    for (const auto& row : rows)
        addressWidth = std::max(addressWidth, static_cast<std::streamsize>(row[0].size()) + 2);

    std::cout << "\n" << std::left << std::setw(addressWidth) << "ADDRESS" << std::setw(6)
              << "RSSI" << "NAME\n";

    for (const auto& row : rows)
        std::cout << std::left << std::setw(addressWidth) << row[0] << std::setw(6) << row[1]
                  << row[2] << "\n";

    const int count = static_cast<int>(rows.size());

    std::cout << "\n" << count << " Bluetooth device(s)";
    if (connectedCount > 0)
        std::cout << ", " << connectedCount << " already connected to this host (RSSI \"-\")";
    std::cout << ". Connect with:\n"
              << "  bt_cli connect bluetooth://<address>\n";

    // Without this an empty table reads as a broken module rather than a working filter. Only the
    // environment variable is checked, so the hint disappears when the user has already opted out
    // that way; a JSON provider could also turn the filter off, which this cannot see.
    if (!filterDisabledByEnvironment())
        std::cout << "\nOnly peripherals advertising -- or, if connected to this host, exposing --\n"
                  << "a service this module has a profile for -- heart rate, cycling power,\n"
                  << "cycling speed and cadence, fitness machine, health thermometer,\n"
                  << "environmental sensing -- are listed. To list\n"
                  << "everything the adapter can hear, set\n"
                  << "  " << FilterEnvVariable << "=false\n";

    return count > 0 ? 0 : 1;
}

/*! The device's ConnectionStatus, or "?" on a device that publishes none. */
std::string connectionStatus(const daq::DevicePtr& device)
{
    try
    {
        const auto status = device.getStatusContainer().getStatus("ConnectionStatus");
        if (status.assigned())
            return static_cast<std::string>(status.toString());
    }
    catch (const std::exception&)
    {
    }

    return "?";
}

/*!
 * Prints the device's GATT property tree: one object property per service, and inside it one
 * property per characteristic whose value is the characteristic's value.
 *
 * The UUID and the GATT flags live in each property's description -- an object property can carry
 * no metadata beyond name, description and read-only, and a name is the addressing key now, so
 * there is nowhere else for them to be.
 */
void printGattTree(const DevicePtr& device)
{
    bool printedHeading = false;

    for (const auto& property : device.getAllProperties())
    {
        // Every object property on a BluetoothDevice is a GATT service.
        if (property.getValueType() != daq::ctObject)
            continue;

        if (!printedHeading)
        {
            std::cout << "GATT database:\n";
            printedHeading = true;
        }

        const std::string serviceName = property.getName();
        std::cout << "  - " << serviceName << "  [" << property.getDescription() << "]\n";

        const PropertyObjectPtr service = device.getPropertyValue(serviceName);
        for (const auto& characteristic : service.getAllProperties())
        {
            const std::string name = characteristic.getName();
            const auto value = service.getPropertyValue(name);

            std::cout << "      " << name;
            if (characteristic.getReadOnly() == daq::False)
                std::cout << "  (writable)";
            std::cout << "\n          " << characteristic.getDescription();

            const auto rendered = value.assigned() ? static_cast<std::string>(value.toString())
                                                   : std::string();
            if (!rendered.empty())
                std::cout << "\n          = " << rendered;
            std::cout << "\n";
        }
    }
}

/*!
 * Resolves what a user typed to a "Service.Characteristic" property path.
 *
 * A full path is used as it stands. Anything else is matched against every characteristic name and
 * UUID, and an ambiguous match is refused rather than guessed at -- the peripheral may expose the
 * same characteristic in two services, and picking one would read somewhere the caller did not
 * intend. Property names removed that ambiguity from the module; this is the one place it survives,
 * because a user still types a short name.
 */
std::string resolveCharacteristicPath(const DevicePtr& device, const std::string& wanted)
{
    if (device.hasProperty(wanted))
        return wanted;

    // Matched textually rather than through the module's UUID helpers: bt_cli links only openDAQ
    // and loads the module from disk, so it cannot call into it. Lowercasing and looking for the
    // text inside the description is enough to make "2a29" find "00002a29-0000-...".
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return text;
    };

    const auto needle = lower(wanted);

    std::vector<std::string> matches;

    for (const auto& property : device.getAllProperties())
    {
        if (property.getValueType() != daq::ctObject)
            continue;

        const std::string serviceName = property.getName();
        const PropertyObjectPtr service = device.getPropertyValue(serviceName);

        for (const auto& characteristic : service.getAllProperties())
        {
            const std::string name = characteristic.getName();
            const std::string description = characteristic.getDescription();

            if (name == wanted || lower(description).find(needle) != std::string::npos)
                matches.push_back(serviceName + "." + name);
        }
    }

    if (matches.empty())
        throw std::runtime_error("No characteristic called \"" + wanted +
                                 "\". Run 'connect' to list them.");

    if (matches.size() > 1)
    {
        std::string message = "\"" + wanted + "\" matches more than one characteristic:";
        for (const auto& match : matches)
            message += "\n  " + match;
        throw std::runtime_error(message + "\nAddress it by its full path instead.");
    }

    return matches.front();
}

int connect(const std::string& connectionString)
{
    const auto instance = createInstance();

    // Discovery has to have heard the peripheral before a connection can be attempted.
    instance.getAvailableDevices();

    std::cout << "Connecting to " << connectionString << "...\n";
    const auto device = instance.addDevice(connectionString);

    std::cout << "\nConnected: " << static_cast<std::string>(device.getName()) << "\n"
              << "  Address : " << propertyOr(device, "Address", "?") << "\n"
              << "  MTU     : " << propertyOr(device, "MtuSize", "?") << "\n"
              << "  Link    : " << connectionStatus(device) << "\n\n";

    const auto channels = device.getChannels();
    if (channels.getCount() > 0)
    {
        std::cout << "Channels (decoded profiles):\n";
        for (const auto& channel : channels)
        {
            std::cout << "  - " << static_cast<std::string>(channel.getName()) << "\n";
            for (const auto& signal : channel.getSignals())
            {
                const auto descriptor = signal.getDescriptor();
                std::cout << "      " << static_cast<std::string>(signal.getLocalId());
                if (descriptor.assigned() && descriptor.getUnit().assigned())
                    std::cout << "  [" << static_cast<std::string>(descriptor.getUnit().getSymbol()) << "]";
                std::cout << "\n";
            }
        }
        std::cout << "\n  Stream them with: bt_cli monitor " << connectionString << "\n\n";
    }

    printGattTree(device);

    return 0;
}

int readCharacteristic(const std::string& connectionString, const std::string& characteristic)
{
    const auto instance = createInstance();
    instance.getAvailableDevices();

    const auto device = instance.addDevice(connectionString);
    const auto path = resolveCharacteristicPath(device, characteristic);

    // Reading serves the value the device already has and schedules a re-read in the background,
    // because a property read holds openDAQ's config lock and nothing holding it may block on the
    // radio. Show both, so a stale figure is visibly stale.
    const auto before = device.getPropertyValue(path);
    std::cout << path << " = " << static_cast<std::string>(before.toString()) << "\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(750));

    const auto after = device.getPropertyValue(path);
    if (static_cast<std::string>(after.toString()) != static_cast<std::string>(before.toString()))
        std::cout << path << " = " << static_cast<std::string>(after.toString()) << "  (refreshed)\n";

    return 0;
}

int writeCharacteristic(const std::string& connectionString,
                        const std::string& characteristic,
                        const std::string& value)
{
    const auto instance = createInstance();
    instance.getAvailableDevices();

    const auto device = instance.addDevice(connectionString);
    const auto path = resolveCharacteristicPath(device, characteristic);

    // The property's declared type decides how the text is interpreted: a characteristic the SIG
    // gives a scalar format takes a number, everything else takes hex bytes.
    const auto type = device.getProperty(path).getValueType();
    if (type == daq::ctInt)
        device.setPropertyValue(path, daq::Integer(std::stoll(value)));
    else
        device.setPropertyValue(path, daq::String(value));

    std::cout << path << " = " << static_cast<std::string>(device.getPropertyValue(path).toString())
              << "\n";
    return 0;
}

/*!
 * Connects and streams every signal of every channel for a while.
 *
 * This reads through openDAQ's own StreamReader rather than tapping the module directly, so it
 * exercises the whole path an application would use: notification -> decode -> packet -> signal.
 *
 * @p channelFilter, when non-empty, limits the output to one channel's local id. A smart trainer
 * hosts four profiles at once and around forty public signals, which without a filter is an
 * unreadable wall at 4 Hz.
 */
int monitor(const std::string& connectionString, int seconds, const std::string& channelFilter)
{
    const auto instance = createInstance();
    instance.getAvailableDevices();

    std::cout << "Connecting to " << connectionString << "...\n";
    const auto device = instance.addDevice(connectionString);

    struct Stream
    {
        std::string label;
        std::string unit;
        StreamReaderPtr reader;
    };

    std::vector<Stream> streams;

    for (const auto& channel : device.getChannels())
    {
        const auto channelId = static_cast<std::string>(channel.getLocalId());
        if (!channelFilter.empty() && channelId != channelFilter)
            continue;

        std::cout << "\n" << channelId << "\n";

        // Descriptive state first, whatever the profile calls it: sensor location, training
        // status, feature lists. Callables are skipped -- they are actions, not readings.
        for (const auto& property : channel.getVisibleProperties())
        {
            if (property.getCallableInfo().assigned())
                continue;

            std::cout << "  " << static_cast<std::string>(property.getName()) << " = "
                      << static_cast<std::string>(channel.getPropertyValue(property.getName()))
                      << "\n";
        }

        // getSignals() is visible-only, and domain signals are created invisible, so this is
        // exactly the value signals with no per-profile knowledge in the CLI.
        for (const auto& signal : channel.getSignals())
        {
            const auto descriptor = signal.getDescriptor();

            std::string unit;
            if (descriptor.assigned() && descriptor.getUnit().assigned())
                unit = static_cast<std::string>(descriptor.getUnit().getSymbol());

            streams.push_back({channelId + "/" + static_cast<std::string>(signal.getLocalId()),
                               unit,
                               StreamReaderBuilder()
                                   .setSignal(signal)
                                   .setValueReadType(SampleType::Float64)
                                   .setDomainReadType(SampleType::Int64)
                                   .setSkipEvents(True)
                                   .build()});
        }
    }

    if (streams.empty())
    {
        if (channelFilter.empty())
            std::cerr << "This device exposes no channels this module has a profile for.\n";
        else
            std::cerr << "This device exposes no channel called " << channelFilter << ".\n";
        return 1;
    }

    std::cout << "\nStreaming " << streams.size() << " signal(s) for " << seconds << "s...\n\n";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int samples = 0;
    bool linkUp = true;

    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        // Without this a dropout is indistinguishable from a sensor that stopped reporting. The
        // readers below are deliberately not rebuilt: they stay attached to the same signals
        // across the outage, which is the point of reconnecting rather than re-adding the device.
        const bool up = boolPropertyOr(device, "Connected", false);
        if (up != linkUp)
        {
            linkUp = up;
            std::cout << (up ? "  -- reconnected --\n"
                             : "  -- link lost, reconnecting --\n");
        }

        for (auto& stream : streams)
        {
            double values[64] = {};
            int64_t times[64] = {};

            daq::SizeT count = 64;
            stream.reader.readWithDomain(values, times, &count);

            for (daq::SizeT i = 0; i < count; ++i)
            {
                ++samples;
                std::cout << "  t=" << times[i] << "us  " << stream.label << " " << values[i];
                if (!stream.unit.empty())
                    std::cout << " " << stream.unit;
                std::cout << "\n";
            }
        }
    }

    std::cout << "\n" << samples << " sample(s).\n";
    return samples > 0 ? 0 : 1;
}

int usage()
{
    std::cerr << "usage:\n"
              << "  bt_cli scan [seconds]\n"
              << "  bt_cli connect <bluetooth://address>\n"
              << "  bt_cli read <bluetooth://address> <characteristic>\n"
              << "  bt_cli write <bluetooth://address> <characteristic> <value>\n"
              << "  bt_cli monitor <bluetooth://address> [seconds] [channel-local-id]\n"
              << "\n"
              << "<characteristic> is a property path such as \"Battery Service.Battery Level\",\n"
              << "or just its name or UUID when that is unambiguous.\n";
    return 2;
}

}

int main(int argc, char** argv)
{
    if (argc < 2)
        return usage();

    const std::string command = argv[1];

    try
    {
        if (command == "scan")
            return scan(argc > 2 ? std::stoi(argv[2]) : 5);

        if (command == "connect" && argc > 2)
            return connect(argv[2]);

        if (command == "write" && argc > 4)
            return writeCharacteristic(argv[2], argv[3], argv[4]);

        if (command == "read" && argc > 3)
            return readCharacteristic(argv[2], argv[3]);

        if (command == "monitor" && argc > 2)
            return monitor(argv[2], argc > 3 ? std::stoi(argv[3]) : 20, argc > 4 ? argv[4] : "");

        return usage();
    }
    catch (const std::exception& e)
    {
        // openDAQ consumes its thread-local error info when it builds the exception, so what
        // reaches here is only the outermost wrapper message. The precise cause -- adapter off,
        // pairing required, peripheral out of range -- is logged by the module itself at error
        // level, which openDAQ prints above this line.
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
