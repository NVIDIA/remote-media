#include "configuration.hpp"
#include "logger.hpp"
#include "state_machine.hpp"
#include "system.hpp"

#include <sys/mount.h>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/process.hpp>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

class App
{
  public:
    App(boost::asio::io_context& ioc, const Configuration& config,
        sd_bus* custom_bus = nullptr) :
        ioc(ioc),
        devMonitor(ioc), config(config)
    {
        if (!custom_bus)
        {
            bus = std::make_shared<sdbusplus::asio::connection>(ioc);
        }
        else
        {
            bus =
                std::make_shared<sdbusplus::asio::connection>(ioc, custom_bus);
        }
        objServer = std::make_shared<sdbusplus::asio::object_server>(bus);
        bus->request_name("xyz.openbmc_project.VirtualMedia");
        objManager = std::make_shared<sdbusplus::server::manager::manager>(
            *bus, "/xyz/openbmc_project/VirtualMedia");

        for (const auto& [name, entry] : config.mountPoints)
        {
            mpsm[name] = std::make_shared<MountPointStateMachine>(
                ioc, devMonitor, name, entry, bus);
            mpsm[name]->emitRegisterDBusEvent(objServer);
        }

        devMonitor.run([this](const NBDDevice& device, StateChange change) {
            for (auto& [name, entry] : mpsm)
            {
                entry->emitUdevStateChangeEvent(device, change);
            }
        });
    }

  private:
    boost::container::flat_map<std::string,
                               std::shared_ptr<MountPointStateMachine>>
        mpsm;
    boost::asio::io_context& ioc;
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> objServer;
    std::shared_ptr<sdbusplus::server::manager::manager> objManager;
    DeviceMonitor devMonitor;
    const Configuration& config;
};

int main()
{
    Configuration config("/etc/virtual-media.json");
    if (!config.valid)
        return -1;

    boost::asio::io_context ioc;
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait(
        [&ioc](const boost::system::error_code&, const int&) { ioc.stop(); });

    sd_bus* b = nullptr;
#if defined(CUSTOM_DBUS_PATH)
#pragma message("You are using custom DBUS path set to " CUSTOM_DBUS_PATH)
    sd_bus_new(&b);
    sd_bus_set_bus_client(b, true);
    sd_bus_set_address(b, CUSTOM_DBUS_PATH);
    sd_bus_start(b);
#endif
    sd_bus_default_system(&b);
    App app(ioc, config, b);

    ioc.run();

    return 0;
}
