#include "bluez_bus.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace opendaq_bt::ble::bluez
{

namespace
{

constexpr char BluezService[] = "org.bluez";
constexpr uint64_t DefaultTimeoutUsec = 30ull * 1000 * 1000;

std::string errorText(const sd_bus_error& err, int r)
{
    if (err.message != nullptr)
        return std::string(err.name ? err.name : "") + ": " + err.message;

    return std::strerror(r < 0 ? -r : r);
}

}

Bus::Bus() = default;

Bus::~Bus()
{
    close();
}

bool Bus::open(std::string& error)
{
    if (bus != nullptr)
        return true;

    const int r = sd_bus_default_system(&bus);
    if (r < 0)
    {
        bus = nullptr;
        error = std::string("cannot connect to the system D-Bus: ") + std::strerror(-r);
        return false;
    }

    wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeFd < 0)
    {
        error = std::string("cannot create an eventfd: ") + std::strerror(errno);
        close();
        return false;
    }

    return true;
}

void Bus::close()
{
    stopDispatch();

    {
        std::scoped_lock guard(mutex);

        for (auto* slot : slots)
            sd_bus_slot_unref(slot);
        slots.clear();

        if (bus != nullptr)
        {
            sd_bus_flush(bus);
            sd_bus_unref(bus);
            bus = nullptr;
        }
    }

    if (wakeFd >= 0)
    {
        ::close(wakeFd);
        wakeFd = -1;
    }
}

bool Bus::callVoid(const std::string& path,
                   const std::string& iface,
                   const std::string& member,
                   std::string& error,
                   uint64_t timeoutUsec)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    int r;
    {
        std::scoped_lock guard(mutex);
        if (bus == nullptr)
        {
            error = "bus is not connected";
            return false;
        }

        sd_bus_message* request = nullptr;
        r = sd_bus_message_new_method_call(
            bus, &request, BluezService, path.c_str(), iface.c_str(), member.c_str());
        if (r >= 0)
        {
            r = sd_bus_call(bus,
                            request,
                            timeoutUsec != 0 ? timeoutUsec : DefaultTimeoutUsec,
                            &err,
                            &reply);
        }
        sd_bus_message_unref(request);
    }

    if (r < 0)
        error = errorText(err, r);

    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);

    return r >= 0;
}

bool Bus::getManagedObjects(ManagedObjects& objects, std::string& error)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    int r;
    {
        std::scoped_lock guard(mutex);
        if (bus == nullptr)
        {
            error = "bus is not connected";
            return false;
        }

        r = sd_bus_call_method(bus,
                               BluezService,
                               "/",
                               "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects",
                               &err,
                               &reply,
                               "");
    }

    if (r < 0)
    {
        error = errorText(err, r);
        sd_bus_error_free(&err);
        return false;
    }

    objects.clear();

    // Reply signature: a{oa{sa{sv}}}
    r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}");
    while (r >= 0 &&
           sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}") > 0)
    {
        const char* objectPath = nullptr;
        if (sd_bus_message_read_basic(reply, SD_BUS_TYPE_OBJECT_PATH, &objectPath) < 0)
            break;

        auto& interfaces = objects[objectPath];

        if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sa{sv}}") < 0)
            break;

        while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}") > 0)
        {
            const char* interfaceName = nullptr;
            if (sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &interfaceName) < 0)
                break;

            readProperties(reply, interfaces[interfaceName]);
            sd_bus_message_exit_container(reply);
        }

        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply);

    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);

    return true;
}

bool Bus::getProperty(const std::string& path,
                      const std::string& iface,
                      const std::string& name,
                      Value& value,
                      std::string& error)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    int r;
    {
        std::scoped_lock guard(mutex);
        if (bus == nullptr)
        {
            error = "bus is not connected";
            return false;
        }

        r = sd_bus_call_method(bus,
                               BluezService,
                               path.c_str(),
                               "org.freedesktop.DBus.Properties",
                               "Get",
                               &err,
                               &reply,
                               "ss",
                               iface.c_str(),
                               name.c_str());
    }

    if (r < 0)
    {
        error = errorText(err, r);
        sd_bus_error_free(&err);
        return false;
    }

    const int read = readVariant(reply, value);

    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);

    if (read < 0)
    {
        error = "cannot decode property " + name;
        return false;
    }

    return true;
}

bool Bus::matchSignal(const std::string& path,
                      const std::string& iface,
                      const std::string& member,
                      sd_bus_message_handler_t handler,
                      void* userdata,
                      std::string& error)
{
    std::scoped_lock guard(mutex);
    if (bus == nullptr)
    {
        error = "bus is not connected";
        return false;
    }

    sd_bus_slot* slot = nullptr;
    const int r = sd_bus_match_signal(bus,
                                      &slot,
                                      BluezService,
                                      path.empty() ? nullptr : path.c_str(),
                                      iface.empty() ? nullptr : iface.c_str(),
                                      member.empty() ? nullptr : member.c_str(),
                                      handler,
                                      userdata);
    if (r < 0)
    {
        error = std::string("cannot install a signal match: ") + std::strerror(-r);
        return false;
    }

    slots.push_back(slot);
    return true;
}

void Bus::startDispatch()
{
    if (running)
        return;

    running = true;
    dispatcher = std::thread([this] { dispatchLoop(); });
}

void Bus::stopDispatch()
{
    if (!running)
        return;

    running = false;
    wake();

    if (dispatcher.joinable())
        dispatcher.join();
}

void Bus::wake()
{
    if (wakeFd < 0)
        return;

    const uint64_t one = 1;
    [[maybe_unused]] const ssize_t written = ::write(wakeFd, &one, sizeof(one));
}

void Bus::dispatchLoop()
{
    while (running)
    {
        int busFd;
        int events;
        uint64_t timeoutUsec = 0;

        {
            std::scoped_lock guard(mutex);
            if (bus == nullptr)
                return;

            // Drain everything already buffered before deciding what to wait for.
            while (sd_bus_process(bus, nullptr) > 0)
            {
                if (!running)
                    return;
            }

            busFd = sd_bus_get_fd(bus);
            events = sd_bus_get_events(bus);
            if (sd_bus_get_timeout(bus, &timeoutUsec) < 0)
                timeoutUsec = UINT64_MAX;
        }

        if (busFd < 0 || events < 0)
            return;

        // Wait outside the lock so a concurrent blocking call (Connect, ReadValue) is not
        // serialised behind the poll timeout.
        pollfd fds[2];
        fds[0].fd = busFd;
        fds[0].events = static_cast<short>(events);
        fds[0].revents = 0;
        fds[1].fd = wakeFd;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int timeoutMs = 200;
        if (timeoutUsec != UINT64_MAX)
            timeoutMs = std::min<int>(timeoutMs, static_cast<int>(timeoutUsec / 1000));

        const int r = ::poll(fds, wakeFd >= 0 ? 2 : 1, timeoutMs);
        if (r < 0 && errno != EINTR)
            return;

        if (wakeFd >= 0 && (fds[1].revents & POLLIN) != 0)
        {
            uint64_t drained = 0;
            [[maybe_unused]] const ssize_t got = ::read(wakeFd, &drained, sizeof(drained));
        }
    }
}

int readVariant(sd_bus_message* m, Value& value)
{
    const char* contents = nullptr;
    char type = 0;

    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r < 0)
        return r;

    if (type != SD_BUS_TYPE_VARIANT || contents == nullptr)
        return -EINVAL;

    const std::string signature = contents;

    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
    if (r < 0)
        return r;

    value = Value{};
    value.type = signature.empty() ? 0 : signature[0];

    if (signature == "s" || signature == "o" || signature == "g")
    {
        const char* s = nullptr;
        r = sd_bus_message_read_basic(m, signature[0], &s);
        if (r >= 0 && s != nullptr)
            value.str = s;
    }
    else if (signature == "b")
    {
        int b = 0;
        r = sd_bus_message_read_basic(m, SD_BUS_TYPE_BOOLEAN, &b);
        value.boolean = b != 0;
        value.integer = b;
    }
    else if (signature == "y")
    {
        uint8_t v = 0;
        r = sd_bus_message_read_basic(m, SD_BUS_TYPE_BYTE, &v);
        value.integer = v;
    }
    else if (signature == "n" || signature == "q")
    {
        // RSSI arrives as a signed 16-bit integer, so sign matters here.
        int16_t v = 0;
        r = sd_bus_message_read_basic(m, signature[0], &v);
        value.integer = signature == "n" ? v : static_cast<uint16_t>(v);
    }
    else if (signature == "i" || signature == "u")
    {
        int32_t v = 0;
        r = sd_bus_message_read_basic(m, signature[0], &v);
        value.integer = signature == "i" ? v : static_cast<uint32_t>(v);
    }
    else if (signature == "x" || signature == "t")
    {
        int64_t v = 0;
        r = sd_bus_message_read_basic(m, signature[0], &v);
        value.integer = v;
    }
    else if (signature == "d")
    {
        double v = 0;
        r = sd_bus_message_read_basic(m, SD_BUS_TYPE_DOUBLE, &v);
        value.integer = static_cast<int64_t>(v);
    }
    else if (signature == "as" || signature == "ao")
    {
        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, signature.c_str() + 1);
        if (r >= 0)
        {
            const char* s = nullptr;
            while (sd_bus_message_read_basic(m, signature[1], &s) > 0)
                value.strings.emplace_back(s != nullptr ? s : "");
            sd_bus_message_exit_container(m);
        }
    }
    else if (signature == "ay")
    {
        const void* data = nullptr;
        size_t size = 0;
        r = sd_bus_message_read_array(m, SD_BUS_TYPE_BYTE, &data, &size);
        if (r >= 0 && data != nullptr)
        {
            const auto* first = static_cast<const uint8_t*>(data);
            value.bytes.assign(first, first + size);
        }
    }
    else if (signature == "a{qv}")
    {
        // ManufacturerData: company id -> byte blob.
        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{qv}");
        if (r >= 0)
        {
            while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "qv") > 0)
            {
                uint16_t key = 0;
                if (sd_bus_message_read_basic(m, SD_BUS_TYPE_UINT16, &key) >= 0)
                {
                    Value inner;
                    if (readVariant(m, inner) >= 0)
                        value.byteArraysByKey[key] = inner.bytes;
                    else
                        sd_bus_message_skip(m, "v");
                }
                sd_bus_message_exit_container(m);
            }
            sd_bus_message_exit_container(m);
        }
    }
    else
    {
        // Not a shape we consume; skip it so parsing of the enclosing container can continue.
        sd_bus_message_skip(m, nullptr);
    }

    sd_bus_message_exit_container(m);
    return r < 0 ? r : 1;
}

int readProperties(sd_bus_message* m, Properties& properties)
{
    int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0)
        return r;

    while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0)
    {
        const char* key = nullptr;
        if (sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &key) < 0)
        {
            sd_bus_message_exit_container(m);
            break;
        }

        Value value;
        if (readVariant(m, value) >= 0 && key != nullptr)
            properties[key] = std::move(value);

        sd_bus_message_exit_container(m);
    }

    sd_bus_message_exit_container(m);
    return 1;
}

std::string addressToNodeName(const std::string& address)
{
    std::string node = "dev_";
    node.reserve(node.size() + address.size());

    for (const char c : address)
        node.push_back(c == ':' ? '_' : c);

    return node;
}

uint32_t flagsToProperties(const std::vector<std::string>& flags)
{
    uint32_t properties = CharPropNone;

    for (const auto& flag : flags)
    {
        if (flag == "read")
            properties |= CharPropRead;
        else if (flag == "write")
            properties |= CharPropWrite;
        else if (flag == "write-without-response")
            properties |= CharPropWriteWithoutResponse;
        else if (flag == "notify")
            properties |= CharPropNotify;
        else if (flag == "indicate")
            properties |= CharPropIndicate;
        else if (flag == "broadcast")
            properties |= CharPropBroadcast;
        else if (flag == "authenticated-signed-writes")
            properties |= CharPropAuthenticatedSignedWrites;
        else if (flag == "extended-properties")
            properties |= CharPropExtendedProperties;
    }

    return properties;
}

}
