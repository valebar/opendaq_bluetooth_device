#include <bluetooth_module/profiles/cycling_common.h>

namespace opendaq_bt::profiles
{

const char* cyclingSensorLocationName(uint8_t value)
{
    switch (value)
    {
        case 0: return "Other";
        case 1: return "Top of Shoe";
        case 2: return "In Shoe";
        case 3: return "Hip";
        case 4: return "Front Wheel";
        case 5: return "Left Crank";
        case 6: return "Right Crank";
        case 7: return "Left Pedal";
        case 8: return "Right Pedal";
        case 9: return "Front Hub";
        case 10: return "Rear Dropout";
        case 11: return "Chainstay";
        case 12: return "Rear Wheel";
        case 13: return "Rear Hub";
        case 14: return "Chest";
        case 15: return "Spider";
        case 16: return "Chain Ring";
        default: return "Unknown";
    }
}

std::string describeBits(uint32_t bits, const char* const* names, size_t nameCount)
{
    std::string result;

    for (size_t i = 0; i < nameCount; ++i)
    {
        if ((bits & (1u << i)) == 0)
            continue;

        if (!result.empty())
            result += ", ";
        result += names[i];
    }

    return result.empty() ? std::string("None") : result;
}

}
