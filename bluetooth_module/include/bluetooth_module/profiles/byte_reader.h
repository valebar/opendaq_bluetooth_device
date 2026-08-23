#pragma once

#include <cstdint>
#include <vector>

namespace opendaq_bt::profiles
{

/*!
 * A little-endian cursor over a GATT characteristic payload.
 *
 * Every field of every characteristic decoded in this module is little-endian and variable in
 * position, because optional fields are gated by flag bits. Bounds-checking each read at the call
 * site turns a decoder into a wall of early returns; instead a read past the end sets a sticky
 * @c overrun flag and yields zero, so a decoder performs all its reads and asks once, at the end,
 * whether the payload actually held them. The reader never reads outside the buffer either way.
 *
 * That check is not optional: real peripherals do send frames whose flags promise more than the
 * frame contains, and a decoder that trusted the flags would read whatever followed in memory.
 */
class ByteReader
{
public:
    explicit ByteReader(const std::vector<uint8_t>& bytes)
        : bytes(bytes)
    {
    }

    /*! True once any read has run past the end of the payload. Check before trusting a result. */
    bool overran() const { return overrun; }

    size_t remaining() const { return overrun ? 0 : bytes.size() - offset; }

    uint8_t u8()
    {
        if (!take(1))
            return 0;
        return bytes[offset - 1];
    }

    int8_t s8() { return static_cast<int8_t>(u8()); }

    uint16_t u16()
    {
        if (!take(2))
            return 0;
        return static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset - 2]) |
                                     static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset - 1]) << 8));
    }

    int16_t s16() { return static_cast<int16_t>(u16()); }

    /*! 24-bit unsigned, as used by Total Distance and the packed extreme angles. */
    uint32_t u24()
    {
        if (!take(3))
            return 0;
        return static_cast<uint32_t>(bytes[offset - 3]) |
               (static_cast<uint32_t>(bytes[offset - 2]) << 8) |
               (static_cast<uint32_t>(bytes[offset - 1]) << 16);
    }

    uint32_t u32()
    {
        if (!take(4))
            return 0;
        return static_cast<uint32_t>(bytes[offset - 4]) |
               (static_cast<uint32_t>(bytes[offset - 3]) << 8) |
               (static_cast<uint32_t>(bytes[offset - 2]) << 16) |
               (static_cast<uint32_t>(bytes[offset - 1]) << 24);
    }

private:
    bool take(size_t count)
    {
        if (overrun || offset + count > bytes.size())
        {
            overrun = true;
            return false;
        }

        offset += count;
        return true;
    }

    const std::vector<uint8_t>& bytes;
    size_t offset = 0;
    bool overrun = false;
};

}
