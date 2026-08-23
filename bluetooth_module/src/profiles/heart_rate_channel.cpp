#include <bluetooth_module/ble/ble_types.h>
#include <bluetooth_module/profiles/heart_rate_channel.h>
#include <bluetooth_module/profiles/heart_rate_measurement.h>

#include <coreobjects/callable_info_factory.h>
#include <coreobjects/property_object_protected_ptr.h>
#include <coreobjects/unit_factory.h>
#include <opendaq/custom_log.h>
#include <opendaq/data_rule_factory.h>
#include <opendaq/packet_factory.h>
#include <opendaq/signal_factory.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <ctime>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ_BLUETOOTH

namespace ble = ::opendaq_bt::ble;
using namespace std::chrono;

namespace
{

/*! Domain ticks are microseconds, which is ample for a signal that updates about once a second. */
constexpr int64_t TicksPerSecond = 1000000;

RatioPtr tickResolution()
{
    return Ratio(1, TicksPerSecond);
}

/*! openDAQ expects an ISO-8601 origin; every timestamp we emit is relative to the Unix epoch. */
StringPtr epochOrigin()
{
    return "1970-01-01T00:00:00Z";
}

/*!
 * Expected spacing between samples, in ticks. Heart Rate Measurement is specified to notify about
 * once per second; the bounds are deliberately loose because RR intervals within one notification
 * are far closer together and a resting subject may exceed a second between beats.
 */
constexpr int64_t MinExpectedDeltaTicks = TicksPerSecond / 20;   // 50 ms, a 1200 bpm ceiling
constexpr int64_t MaxExpectedDeltaTicks = TicksPerSecond * 5;    // 5 s

microseconds currentEpochOffset()
{
    return duration_cast<microseconds>(system_clock::now().time_since_epoch());
}

/*! Finds a characteristic by UUID within a service, or returns an empty handle. */
std::string handleOf(const ble::Service& service, const char* uuid)
{
    const auto wanted = ble::expandUuid(uuid);

    for (const auto& characteristic : service.characteristics)
    {
        if (ble::expandUuid(characteristic.uuid) == wanted)
            return characteristic.handle;
    }

    return {};
}

const ble::Characteristic* characteristicOf(const ble::Service& service, const char* uuid)
{
    const auto wanted = ble::expandUuid(uuid);

    for (const auto& characteristic : service.characteristics)
    {
        if (ble::expandUuid(characteristic.uuid) == wanted)
            return &characteristic;
    }

    return nullptr;
}

}

const char* HeartRateChannel::serviceUuid()
{
    return "0000180d-0000-1000-8000-00805f9b34fb";
}

const char* HeartRateChannel::measurementCharacteristicUuid()
{
    return "00002a37-0000-1000-8000-00805f9b34fb";
}

const char* HeartRateChannel::bodySensorLocationCharacteristicUuid()
{
    return "00002a38-0000-1000-8000-00805f9b34fb";
}

const char* HeartRateChannel::controlPointCharacteristicUuid()
{
    return "00002a39-0000-1000-8000-00805f9b34fb";
}

HeartRateChannel::HeartRateChannel(const ContextPtr& context,
                                   const ComponentPtr& parent,
                                   const StringPtr& localId,
                                   std::shared_ptr<ble::IConnection> connection,
                                   const ble::Service& service)
    : ChannelImpl(FunctionBlockType("BluetoothHeartRate", "Heart Rate", "Bluetooth SIG Heart Rate service"),
                  context,
                  parent,
                  localId)
    , connection(std::move(connection))
    , epochOffset(currentEpochOffset())
    , steadyStart(steady_clock::now())
{
    const auto* measurement = characteristicOf(service, measurementCharacteristicUuid());
    if (measurement == nullptr)
        DAQ_THROW_EXCEPTION(NotFoundException,
                            "The Heart Rate service does not expose a Heart Rate Measurement characteristic.");

    // Without notifications the service can only be polled, which is not what this channel is for.
    if ((measurement->properties & (ble::CharPropNotify | ble::CharPropIndicate)) == 0)
        DAQ_THROW_EXCEPTION(InvalidStateException,
                            "The Heart Rate Measurement characteristic does not support notifications.");

    measurementHandle = measurement->handle;
    controlPointHandle = handleOf(service, controlPointCharacteristicUuid());

    initProperties(service);
    createSignals();
    buildSignalDescriptors();

    this->connection->subscribe(measurementHandle,
                         [this](const std::vector<uint8_t>& payload) { onMeasurement(payload); });
}

HeartRateChannel::~HeartRateChannel()
{
    // Stop the peripheral notifying before the signals go away, so no packet is built against a
    // half-destroyed channel.
    try
    {
        connection->unsubscribe(measurementHandle);
    }
    catch (const std::exception&)
    {
        // The link may already be down; there is nothing useful to do about it during teardown.
    }
}

void HeartRateChannel::initProperties(const ble::Service& service)
{
    const auto objPtr = this->template borrowPtr<PropertyObjectPtr>();

    // Mirrors of peripheral state: read-only to applications, updated internally through the
    // protected setter as notifications arrive.
    objPtr.addProperty(BoolPropertyBuilder("SensorContactSupported", False).setReadOnly(True).build());
    objPtr.addProperty(BoolPropertyBuilder("SensorContactDetected", False).setReadOnly(True).build());
    objPtr.addProperty(StringPropertyBuilder("BodySensorLocation", "Unknown").setReadOnly(True).build());

    const auto locationHandle = handleOf(service, bodySensorLocationCharacteristicUuid());
    if (!locationHandle.empty())
        readBodySensorLocation(locationHandle);

    // The control point is optional, so the reset action only exists where the peripheral can act
    // on it -- an always-present button that always fails would be worse than none.
    if (!controlPointHandle.empty())
    {
        objPtr.addProperty(FunctionProperty("ResetEnergyExpended", ProcedureInfo(List<IArgumentInfo>())));
        // Function/procedure properties are protected in some openDAQ builds (Dewesoft's prebuilt
        // 3.40.0.12.x among them), where the public setPropertyValue() rejects the callable with
        // 'Failed to set property value'. openDAQ v3.40.1 accepts either. Use the protected setter
        // so this module builds against both.
        objPtr.asPtr<IPropertyObjectProtected>().setProtectedPropertyValue(
            String("ResetEnergyExpended"),
            Procedure([this] {
                // 0x01 is the "Reset Energy Expended" opcode of the Heart Rate Control Point.
                connection->write(controlPointHandle, std::vector<uint8_t>{0x01}, true);
            }));
    }
}

void HeartRateChannel::readBodySensorLocation(const std::string& handle)
{
    try
    {
        const auto value = connection->read(handle);
        if (value.empty())
            return;

        this->template borrowPtr<PropertyObjectProtectedPtr>().setProtectedPropertyValue(
            String("BodySensorLocation"), String(::opendaq_bt::profiles::bodySensorLocationName(value[0])));
    }
    catch (const std::exception& e)
    {
        // Purely descriptive; a peripheral that refuses the read still streams heart rate.
        LOG_D("Cannot read the body sensor location: {}", e.what())
    }
}

void HeartRateChannel::createSignals()
{
    heartRateSignal = createAndAddSignal("HeartRate");
    heartRateTimeSignal = createAndAddSignal("HeartRateTime", nullptr, false, false);

    rrIntervalSignal = createAndAddSignal("RrInterval");
    rrIntervalTimeSignal = createAndAddSignal("RrIntervalTime", nullptr, false, false);

    energySignal = createAndAddSignal("EnergyExpended");
    energyTimeSignal = createAndAddSignal("EnergyExpendedTime", nullptr, false, false);
}

void HeartRateChannel::buildSignalDescriptors()
{
    const auto domainDescriptor = [](const char* name) {
        return DataDescriptorBuilder()
            .setName(name)
            .setSampleType(SampleType::Int64)
            .setUnit(Unit("s", -1, "seconds", "time"))
            .setTickResolution(tickResolution())
            .setOrigin(epochOrigin())
            .setRule(ExplicitDomainDataRule(MinExpectedDeltaTicks, MaxExpectedDeltaTicks))
            .build();
    };

    heartRateSignal.setDescriptor(DataDescriptorBuilder()
                                      .setName("Heart Rate")
                                      .setSampleType(SampleType::Float64)
                                      .setUnit(Unit("bpm", -1, "beats per minute", "heart rate"))
                                      .setValueRange(Range(0, 300))
                                      .build());
    heartRateTimeSignal.setDescriptor(domainDescriptor("Heart Rate Time"));
    heartRateSignal.setDomainSignal(heartRateTimeSignal);

    rrIntervalSignal.setDescriptor(DataDescriptorBuilder()
                                       .setName("RR Interval")
                                       .setSampleType(SampleType::Float64)
                                       .setUnit(Unit("s", -1, "seconds", "time"))
                                       .build());
    rrIntervalTimeSignal.setDescriptor(domainDescriptor("RR Interval Time"));
    rrIntervalSignal.setDomainSignal(rrIntervalTimeSignal);

    energySignal.setDescriptor(DataDescriptorBuilder()
                                   .setName("Energy Expended")
                                   .setSampleType(SampleType::Float64)
                                   .setUnit(Unit("kJ", -1, "kilojoule", "energy"))
                                   .build());
    energyTimeSignal.setDescriptor(domainDescriptor("Energy Expended Time"));
    energySignal.setDomainSignal(energyTimeSignal);
}

int64_t HeartRateChannel::nowTicks() const
{
    const auto elapsed = duration_cast<microseconds>(steady_clock::now() - steadyStart);
    return (epochOffset + elapsed).count();
}

void HeartRateChannel::onMeasurement(const std::vector<uint8_t>& payload)
{
    const auto decoded = ::opendaq_bt::profiles::decodeHeartRateMeasurement(payload);
    if (!decoded)
    {
        LOG_W("Discarding a malformed Heart Rate Measurement of {} byte(s)", payload.size())
        return;
    }

    const auto arrival = nowTicks();

    std::scoped_lock lock(sync);

    const auto objPtr = this->template borrowPtr<PropertyObjectProtectedPtr>();
    objPtr.setProtectedPropertyValue(String("SensorContactSupported"),
                                     BooleanPtr(decoded->sensorContactSupported));
    objPtr.setProtectedPropertyValue(String("SensorContactDetected"),
                                     BooleanPtr(decoded->sensorContactDetected));

    // ---- heart rate: exactly one sample per notification --------------------------------------
    {
        const auto domainPacket = DataPacket(heartRateTimeSignal.getDescriptor(), 1);
        const auto dataPacket = DataPacketWithDomain(domainPacket, heartRateSignal.getDescriptor(), 1);

        *static_cast<int64_t*>(domainPacket.getRawData()) = arrival;
        *static_cast<double*>(dataPacket.getRawData()) = static_cast<double>(decoded->beatsPerMinute);

        heartRateTimeSignal.sendPacket(domainPacket);
        heartRateSignal.sendPacket(dataPacket);
    }

    // ---- RR intervals: zero or more, back-dated from the arrival time --------------------------
    if (!decoded->rrIntervalsSeconds.empty())
    {
        const auto count = decoded->rrIntervalsSeconds.size();

        const auto domainPacket = DataPacket(rrIntervalTimeSignal.getDescriptor(), count);
        const auto dataPacket = DataPacketWithDomain(domainPacket, rrIntervalSignal.getDescriptor(), count);

        auto* times = static_cast<int64_t*>(domainPacket.getRawData());
        auto* values = static_cast<double*>(dataPacket.getRawData());

        // Each value is the interval that *ended* at its beat, and the newest beat is the one that
        // triggered this notification. Walking the total backwards puts the last beat at the
        // arrival time and reconstructs the earlier ones, which is the standard approach: BLE
        // carries no device-side timestamps, so this is the best timing available to a host.
        double elapsed = 0.0;
        for (const auto interval : decoded->rrIntervalsSeconds)
            elapsed += interval;

        int64_t cursor = arrival - static_cast<int64_t>(elapsed * TicksPerSecond);
        for (size_t i = 0; i < count; ++i)
        {
            const auto interval = decoded->rrIntervalsSeconds[i];
            cursor += static_cast<int64_t>(interval * TicksPerSecond);

            times[i] = cursor;
            values[i] = interval;
        }

        rrIntervalTimeSignal.sendPacket(domainPacket);
        rrIntervalSignal.sendPacket(dataPacket);
    }

    // ---- energy expended: only when reported ---------------------------------------------------
    if (decoded->energyExpendedKj)
    {
        const auto domainPacket = DataPacket(energyTimeSignal.getDescriptor(), 1);
        const auto dataPacket = DataPacketWithDomain(domainPacket, energySignal.getDescriptor(), 1);

        *static_cast<int64_t*>(domainPacket.getRawData()) = arrival;
        *static_cast<double*>(dataPacket.getRawData()) = static_cast<double>(*decoded->energyExpendedKj);

        energyTimeSignal.sendPacket(domainPacket);
        energySignal.sendPacket(dataPacket);
    }
}

END_NAMESPACE_OPENDAQ_BLUETOOTH
