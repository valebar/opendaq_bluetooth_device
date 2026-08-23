#include "fake_ble_central.h"

#include <bluetooth_module/bluetooth_device.h>
#include <bluetooth_module/bluetooth_module.h>
#include <bluetooth_module/profiles/heart_rate_channel.h>

#include <gtest/gtest.h>

#include <opendaq/opendaq.h>

#include <chrono>
#include <memory>
#include <thread>

using namespace daq;
using namespace daq::modules::bluetooth_module;

using opendaq_bt::testing::FakeCentral;
using opendaq_bt::testing::FakeConnection;

namespace ble = ::opendaq_bt::ble;

namespace
{

constexpr char Identifier[] = "AA:BB:CC:DD:EE:FF";
constexpr char ConnectionString[] = "bluetooth://AA:BB:CC:DD:EE:FF";

constexpr char MeasurementHandle[] = "/fake/hr/measurement";

/*! The same peripheral with a different set of backend handles, as BlueZ may renumber them. */
FakeCentral::Peripheral heartRatePeripheral(const std::string& handlePrefix = "/fake",
                                            uint16_t mtu = 247)
{
    ble::Characteristic measurement;
    measurement.uuid = "00002a37-0000-1000-8000-00805f9b34fb";
    measurement.handle = handlePrefix + "/hr/measurement";
    measurement.properties = ble::CharPropNotify;

    ble::Service heartRate;
    heartRate.uuid = "0000180d-0000-1000-8000-00805f9b34fb";
    heartRate.primary = true;
    heartRate.characteristics = {measurement};

    ble::Characteristic battery;
    battery.uuid = "00002a19-0000-1000-8000-00805f9b34fb";
    battery.handle = handlePrefix + "/battery/level";
    battery.properties = ble::CharPropRead;

    ble::Service batteryService;
    batteryService.uuid = "0000180f-0000-1000-8000-00805f9b34fb";
    batteryService.primary = true;
    batteryService.characteristics = {battery};

    FakeCentral::Peripheral peripheral;
    peripheral.services = {heartRate, batteryService};
    peripheral.values[handlePrefix + "/battery/level"] = {0x5A};
    peripheral.mtu = mtu;

    return peripheral;
}

/*! Module defaults plus a retry period short enough that a test never waits on a clock. */
PropertyObjectPtr reconnectConfig(bool autoReconnect = true, int intervalMs = 1)
{
    auto config = BluetoothModule::createDefaultConfig();
    config.setPropertyValue("ScanWarmupMs", 0);
    config.setPropertyValue("AutoReconnect", BooleanPtr(autoReconnect));
    config.setPropertyValue("ReconnectIntervalMs", intervalMs);
    return config;
}

struct Connected
{
    ContextPtr context;
    ModulePtr module;
    DevicePtr device;
    FakeCentral* central = nullptr;
};

Connected connectDevice(FakeCentral::Peripheral peripheral, const PropertyObjectPtr& config)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral(Identifier, std::move(peripheral));

    const auto context = NullContext();
    const auto module =
        createWithImplementation<IModule, BluetoothModule>(context, std::move(owned), config);

    return {context, module, module.createDevice(ConnectionString, nullptr, config), central};
}

/*!
 * Polls until @p predicate holds.
 *
 * Reconnection happens on a background thread, so there is an edge that cannot be observed
 * synchronously. This returns the moment the predicate holds -- a passing test costs a millisecond
 * or two -- and the timeout only bounds a genuine failure rather than pacing a passing run.
 */
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

bool isConnected(const DevicePtr& device)
{
    return static_cast<bool>(static_cast<Bool>(device.getPropertyValue("Connected")));
}

std::string componentStatus(const DevicePtr& device)
{
    const auto status = device.getStatusContainer().getStatus("ConnectionStatus");
    return status.assigned() ? static_cast<std::string>(status.toString()) : std::string();
}

std::string configurationStatus(const DevicePtr& device)
{
    const auto status = device.getConnectionStatusContainer().getStatus("ConfigurationStatus");
    return status.assigned() ? static_cast<std::string>(status.toString()) : std::string();
}

ChannelPtr heartRateChannel(const DevicePtr& device)
{
    for (const auto& channel : device.getChannels())
    {
        if (channel.getLocalId() == "HeartRate")
            return channel;
    }
    return nullptr;
}

SignalPtr signalByLocalId(const ChannelPtr& channel, const std::string& localId)
{
    for (const auto& signal : channel.getSignals(search::Any()))
    {
        if (signal.getLocalId() == localId)
            return signal;
    }
    return nullptr;
}

/*! See the note in test_heart_rate_channel.cpp: a plain reader stops at the descriptor event. */
StreamReaderPtr dataReader(const SignalPtr& signal)
{
    return StreamReaderBuilder()
        .setSignal(signal)
        .setValueReadType(SampleType::Float64)
        .setDomainReadType(SampleType::Int64)
        .setSkipEvents(True)
        .build();
}

}

TEST(Reconnect, ReportsConnectedWhenTheDeviceIsBuilt)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig());

    EXPECT_TRUE(isConnected(connected.device));
    EXPECT_EQ(componentStatus(connected.device), "Connected");
    EXPECT_EQ(configurationStatus(connected.device), "Connected");
}

TEST(Reconnect, GoesToReconnectingWhenTheLinkDrops)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig());

    // Out of range before the drop, so the device stays down long enough to be observed.
    connected.central->setReachable(Identifier, false);
    connected.central->connection()->simulateDisconnect();

    // Everything about the drop is now published from the reconnector's own thread: a backend
    // thread holds the connection's bus mutex, and every openDAQ property set takes the device's
    // config lock, so touching openDAQ from there would deadlock against a caller writing a
    // characteristic.
    EXPECT_TRUE(waitFor([&] { return !isConnected(connected.device); }));
    EXPECT_TRUE(waitFor([&] { return componentStatus(connected.device) == "Reconnecting"; }));
    EXPECT_EQ(configurationStatus(connected.device), "Reconnecting");
}

TEST(Reconnect, ReturnsToConnectedWhenThePeripheralComesBack)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig());

    connected.central->setReachable(Identifier, false);
    connected.central->connection()->simulateDisconnect();
    ASSERT_TRUE(waitFor([&] { return !isConnected(connected.device); }));

    connected.central->setReachable(Identifier, true);

    EXPECT_TRUE(waitFor([&] { return isConnected(connected.device); }));
    EXPECT_EQ(componentStatus(connected.device), "Connected");
    EXPECT_EQ(configurationStatus(connected.device), "Connected");
}

TEST(Reconnect, RetriesUntilThePeripheralReturns)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig());

    connected.central->setReachable(Identifier, false);
    connected.central->connection()->simulateDisconnect();

    // Several failed attempts, so a single retry is not mistaken for perseverance.
    EXPECT_TRUE(connected.central->waitForConnectAttempts(4, std::chrono::seconds(5)));
    EXPECT_TRUE(waitFor([&] { return !isConnected(connected.device); }));

    connected.central->setReachable(Identifier, true);
    EXPECT_TRUE(waitFor([&] { return isConnected(connected.device); }));
}

TEST(Reconnect, KeepsStreamingHeartRateAcrossAReconnect)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig());

    const auto channel = heartRateChannel(connected.device);
    ASSERT_TRUE(channel.assigned());

    // Taken before the outage and held across it: this is the whole point of the feature.
    const auto signal = signalByLocalId(channel, "HeartRate");
    ASSERT_TRUE(signal.assigned());
    auto reader = dataReader(signal);

    connected.central->connection()->pushNotification(MeasurementHandle, {0x00, 0x50});

    connected.central->setReachable(Identifier, false);
    connected.central->connection()->simulateDisconnect();
    ASSERT_TRUE(waitFor([&] { return !isConnected(connected.device); }));

    connected.central->setReachable(Identifier, true);
    ASSERT_TRUE(waitFor([&] { return isConnected(connected.device); }));

    // The new connection is a different backend object; the notification still reaches the signal
    // the reader was attached to before the link ever dropped.
    ASSERT_TRUE(waitFor([&] { return connected.central->connection()->isSubscribed(MeasurementHandle); }));
    connected.central->connection()->pushNotification(MeasurementHandle, {0x00, 0x55});

    EXPECT_EQ(signalByLocalId(heartRateChannel(connected.device), "HeartRate"), signal)
        << "The channel's signal was replaced, so every reader a consumer held is dead.";

    double values[8] = {};
    int64_t times[8] = {};
    SizeT count = 8;
    reader.readWithDomain(values, times, &count);

    ASSERT_EQ(count, 2u) << "The reader stopped producing across the outage.";
    EXPECT_DOUBLE_EQ(values[0], 80.0);
    EXPECT_DOUBLE_EQ(values[1], 85.0);
}

TEST(Reconnect, ResubscribesOnTheNewConnection)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig());

    ASSERT_TRUE(connected.central->connection()->isSubscribed(MeasurementHandle));

    connected.central->connection()->simulateDisconnect();

    // Waiting on the new connection rather than on Connected: the whole link state is published
    // asynchronously now, so the property still reads true for a moment after the drop.
    ASSERT_TRUE(waitFor([&] { return connected.central->connectionCount() == 2u; }));
    ASSERT_TRUE(waitFor([&] { return isConnected(connected.device); }));
    EXPECT_TRUE(connected.central->connectionAt(1)->isSubscribed(MeasurementHandle));
}

TEST(Reconnect, ReleasesTheOldConnectionBeforeConnectingAgain)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig());

    connected.central->connection()->simulateDisconnect();
    ASSERT_TRUE(waitFor([&] { return connected.central->connectAttempts() == 2; }));
    ASSERT_TRUE(waitFor([&] { return isConnected(connected.device); }));

    // A BlueZ connection disconnects the peripheral as it is destroyed, and its object path is
    // derived from the MAC -- so a stale one destroyed after the reconnect would tear the new link
    // straight back down.
    EXPECT_TRUE(connected.central->previousWasDestroyedAt(1))
        << "The dead connection outlived the reconnect; on Linux that disconnects the new link.";
}

TEST(Reconnect, KeepsPropertyNamesStableAcrossAReconnect)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig());

    const auto before = connected.device.getPropertyValue("Battery Service.Battery Level");
    ASSERT_EQ(static_cast<Int>(before), 0x5A);

    // The peripheral comes back with every backend handle renumbered, as BlueZ does when the ATT
    // handles move. Names are derived once from the frozen database, so what a user addresses must
    // not move under them.
    auto renumbered = heartRatePeripheral("/renumbered");
    renumbered.values["/renumbered/battery/level"] = {0x2A};
    connected.central->addPeripheral(Identifier, std::move(renumbered));

    connected.central->connection()->simulateDisconnect();
    ASSERT_TRUE(waitFor([&] { return isConnected(connected.device); }));

    EXPECT_TRUE(connected.device.hasProperty("Battery Service"));
    EXPECT_TRUE(connected.device.hasProperty("Heart Rate"));

    // And the reconnector's handle remap is what makes the unchanged name reach the renumbered
    // characteristic: the value refreshes, through the original path, after the link came back.
    EXPECT_TRUE(waitFor([&] {
        return static_cast<Int>(
                   connected.device.getPropertyValue("Battery Service.Battery Level")) == 0x2A;
    }));
}

TEST(Reconnect, ServesTheLastKnownValueWhileTheLinkIsDown)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig());

    ASSERT_EQ(static_cast<Int>(connected.device.getPropertyValue("Battery Service.Battery Level")),
              0x5A);

    connected.central->setReachable(Identifier, false);
    connected.central->connection()->simulateDisconnect();
    ASSERT_TRUE(waitFor([&] { return !isConnected(connected.device); }));

    // Browsing a disconnected device has to keep working: a read that threw would break a client
    // walking the tree, and the last value read is the most truthful thing there is.
    EXPECT_NO_THROW(connected.device.getPropertyValue("Battery Service.Battery Level"));
    EXPECT_EQ(static_cast<Int>(connected.device.getPropertyValue("Battery Service.Battery Level")),
              0x5A);
}

TEST(Reconnect, RefreshesTheMtuAfterAReconnect)
{
    const auto connected = connectDevice(heartRatePeripheral("/fake", 247), reconnectConfig());

    ASSERT_EQ(static_cast<Int>(connected.device.getPropertyValue("MtuSize")), 247);

    // The ATT MTU is renegotiated per link, so the figure from the old one is not the new one.
    connected.central->addPeripheral(Identifier, heartRatePeripheral("/fake", 185));
    connected.central->connection()->simulateDisconnect();

    EXPECT_TRUE(waitFor([&] {
        return static_cast<Int>(connected.device.getPropertyValue("MtuSize")) == 185;
    }));
}

TEST(Reconnect, DoesNotReconnectWhenAutoReconnectIsOff)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig(false));

    connected.central->connection()->simulateDisconnect();

    EXPECT_TRUE(waitFor([&] { return !isConnected(connected.device); }));
    EXPECT_TRUE(waitFor([&] { return componentStatus(connected.device) == "Unrecoverable"; }));
    EXPECT_EQ(configurationStatus(connected.device), "Unrecoverable");

    // Give a supervisor that should not be retrying every chance to prove otherwise.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(connected.central->connectAttempts(), 1);
}

TEST(Reconnect, StopsRetryingWhenTheDeviceIsRemoved)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral(Identifier, heartRatePeripheral());

    const auto config = reconnectConfig();
    const auto module =
        createWithImplementation<IModule, BluetoothModule>(NullContext(), std::move(owned), config);
    const auto device = module.createDevice(ConnectionString, nullptr, config);

    central->setReachable(Identifier, false);
    central->connection()->simulateDisconnect();
    ASSERT_TRUE(central->waitForConnectAttempts(3, std::chrono::seconds(5)));

    device.remove();

    const int afterRemoval = central->connectAttempts();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(central->connectAttempts(), afterRemoval)
        << "The reconnector kept running after the device left the openDAQ tree.";
}

TEST(Reconnect, ReleasesEverythingWhenDestroyedMidReconnect)
{
    auto owned = std::make_unique<FakeCentral>();
    auto* central = owned.get();
    central->addPeripheral(Identifier, heartRatePeripheral());

    std::shared_ptr<bool> destroyed;
    {
        const auto config = reconnectConfig();
        const auto module = createWithImplementation<IModule, BluetoothModule>(
            NullContext(), std::move(owned), config);
        const auto device = module.createDevice(ConnectionString, nullptr, config);

        // The flag has to be taken while the module -- which owns the backend -- is still alive.
        destroyed = central->connectionDestroyedFlag();
        ASSERT_TRUE(destroyed);

        central->setReachable(Identifier, false);
        central->connection()->simulateDisconnect();
        ASSERT_TRUE(central->waitForConnectAttempts(3, std::chrono::seconds(5)));
    }

    // Destroyed while the supervisor was mid-retry: it must stop, join, and let the link go rather
    // than hang or leave the radio connected.
    EXPECT_TRUE(*destroyed);
}

TEST(Reconnect, RetriesWhenASubscriptionCannotBeReapplied)
{
    const auto connected = connectDevice(heartRatePeripheral(), reconnectConfig());

    // Reachable throughout, but every new link refuses notifications, as a peripheral whose bond
    // was wiped does. A link whose subscriptions silently failed would look healthy and never
    // deliver a sample, so the reconnect must not be declared a success.
    connected.central->refuseSubscriptionsFor(MeasurementHandle);
    connected.central->connection()->simulateDisconnect();

    ASSERT_TRUE(connected.central->waitForConnectAttempts(4, std::chrono::seconds(5)));
    EXPECT_FALSE(isConnected(connected.device))
        << "A link whose heart rate subscription failed was reported as connected.";
}

TEST(Reconnect, PublishesTheReconnectSettingsInItsDefaultConfig)
{
    const auto config = BluetoothModule::createDefaultConfig();

    ASSERT_TRUE(config.hasProperty("AutoReconnect"));
    ASSERT_TRUE(config.hasProperty("ReconnectIntervalMs"));

    EXPECT_TRUE(static_cast<Bool>(config.getPropertyValue("AutoReconnect")));
    EXPECT_EQ(static_cast<Int>(config.getPropertyValue("ReconnectIntervalMs")), 5000);
}
