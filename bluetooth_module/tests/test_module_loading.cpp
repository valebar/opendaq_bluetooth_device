// Verifies the parts that only exist once the module is a real shared library on disk: the
// DEFINE_MODULE_EXPORTS entry points, the ".module.<ext>" filename the ModuleManager insists on,
// and the routing of the "bluetooth" connection string prefix through a live openDAQ Instance.
//
// These three are the failure modes that unit tests against the C++ classes cannot catch, because
// a module that is perfectly correct internally is still invisible if it is named wrongly.

#include <gtest/gtest.h>

#include <opendaq/opendaq.h>

#include <string>

using namespace daq;

namespace
{

InstancePtr createInstanceFromBuildOutput()
{
    // OPENDAQ_BT_MODULE_DIR is the directory the module target writes into, injected by CMake.
    return InstanceBuilder().addModulePath(OPENDAQ_BT_MODULE_DIR).build();
}

}

TEST(ModuleLoading, ModuleIsDiscoveredAndLoaded)
{
    const auto instance = createInstanceFromBuildOutput();

    bool found = false;
    for (const auto& module : instance.getModuleManager().getModules())
    {
        if (module.getModuleInfo().getName() == "BluetoothModule")
            found = true;
    }

    ASSERT_TRUE(found) << "The module was not loaded. Check that the built library is named "
                          "*.module.so/.dll/.dylib and exports createModule().";
}

TEST(ModuleLoading, RegistersTheBluetoothDeviceType)
{
    const auto instance = createInstanceFromBuildOutput();

    const auto types = instance.getAvailableDeviceTypes();
    ASSERT_TRUE(types.hasKey("bluetooth_ble"));

    const DeviceTypePtr type = types.get("bluetooth_ble");
    EXPECT_EQ(type.getConnectionStringPrefix(), "bluetooth");
}

TEST(ModuleLoading, RoutesBluetoothConnectionStringsToThisModule)
{
    const auto instance = createInstanceFromBuildOutput();

    // No such peripheral exists, so this must fail -- but it must fail *inside our module*,
    // with our diagnostic, rather than with openDAQ reporting that nothing handles the prefix.
    try
    {
        instance.addDevice("bluetooth://AA:BB:CC:DD:EE:FF");
        FAIL() << "Expected the connection to a nonexistent peripheral to fail.";
    }
    catch (const std::exception& e)
    {
        const std::string message = e.what();
        EXPECT_EQ(message.find("does not support the connection string"), std::string::npos)
            << "openDAQ did not route the prefix to this module: " << message;
    }
}

TEST(ModuleLoading, RejectsMalformedIdentifiersThroughTheInstance)
{
    const auto instance = createInstanceFromBuildOutput();

    ASSERT_ANY_THROW(instance.addDevice("bluetooth://definitely-not-a-device"));
}

TEST(ModuleLoading, DiscoveryRunsWithoutThrowingRegardlessOfHardware)
{
    const auto instance = createInstanceFromBuildOutput();

    // On a machine with no adapter this must return quietly rather than break discovery for
    // every other module in the instance.
    ASSERT_NO_THROW(instance.getAvailableDevices());
}
