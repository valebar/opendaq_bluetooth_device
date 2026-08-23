#include <bluetooth_module/ble/ble_central.h>

#include <gtest/gtest.h>

namespace opendaq_bt::ble
{

/*!
 * The unit tests link the module's objects but deliberately not its platform BLE backend: every
 * test drives a scripted FakeCentral through the injecting constructor, so no test ever touches a
 * radio. Defining the factory here satisfies the linker and turns any accidental use of the
 * production code path into an immediate, obvious failure rather than a silent hardware access.
 */
std::unique_ptr<ICentral> createPlatformCentral()
{
    throw BleError("Unit tests must inject a FakeCentral; the platform backend is not linked in.");
}

}

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
