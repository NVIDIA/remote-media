#pragma once

#include "logger.hpp"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/args.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/async_pipe.hpp>
#include <boost/process/v1/async.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <format>

namespace fs = std::filesystem;

namespace udev
{
#include <libudev.h>
struct udev;
struct udev_monitor;
struct udev_device;

struct udevDeleter
{
    void operator()(udev* desc) const
    {
        udev_unref(desc);
    }
};

struct monitorDeleter
{
    void operator()(udev_monitor* desc) const
    {
        udev_monitor_unref(desc);
    };
};

struct deviceDeleter
{
    void operator()(udev_device* desc) const
    {
        udev_device_unref(desc);
    };
};
} // namespace udev

#define NBD_DISCONNECT _IO(0xab, 8)
#define NBD_CLEAR_SOCK _IO(0xab, 4)

class NBDDevice
{
  public:
    enum Value : uint8_t // This is explicite not a class to avoid naming like
                         // NBDDevice::Device::nbd0
    {
        nbd0 = 0,
        nbd1 = 1,
        nbd2 = 2,
        nbd3 = 3,
        nbd4 = 4,
        nbd5 = 5,
        nbd6 = 6,
        nbd7 = 7,
        nbd8 = 8,
        nbd9 = 9,
        nbd10 = 10,
        unknown = 0xFF
    };

    NBDDevice() = default;
    NBDDevice(Value v) : value(v){};
    explicit NBDDevice(const char* nbdName)
    {
        if (nbdName != nullptr)
        {
            const auto iter =
                std::find(nameMatching.cbegin(), nameMatching.cend(), nbdName);
            if (iter != nameMatching.cend())
            {
                value = static_cast<Value>(
                    std::distance(nameMatching.cbegin(), iter));
            }
        }
    }
    NBDDevice(const NBDDevice&) = default;
    NBDDevice(NBDDevice&&) = default;

    NBDDevice& operator=(const NBDDevice&) = default;
    NBDDevice& operator=(NBDDevice&&) = default;

    bool operator==(const NBDDevice& rhs) const
    {
        return value == rhs.value;
    }
    bool operator!=(const NBDDevice& rhs) const
    {
        return value != rhs.value;
    }
    bool operator<(const NBDDevice& rhs) const
    {
        return value < rhs.value;
    }
    explicit operator bool()
    {
        return (value != unknown);
    }

    bool isReady() const
    {
        if (value == unknown)
        {
            return false;
        }
        int fd = open(to_path().c_str(), O_EXCL);
        if (fd < 0)
        {
            return false;
        }
        close(fd);
        return true;
    }

    void disconnect() const
    {
        if (value == unknown)
        {
            return;
        }

        int fd = open(to_path().c_str(), O_RDWR);

        if (fd < 0)
        {
            LogMsg(Logger::Error, "Couldn't open device ", to_path().c_str());
            return;
        }
        if (ioctl(fd, NBD_DISCONNECT) < 0)
        {
            LogMsg(Logger::Debug, "Ioctl failed: \n");
        }
        if (ioctl(fd, NBD_CLEAR_SOCK) < 0)
        {
            LogMsg(Logger::Debug, "Ioctl failed: \n");
        }
        close(fd);
    }

    std::string to_string() const
    {
        if (value == unknown)
        {
            return "";
        }
        return nameMatching[static_cast<uint8_t>(value)];
    }

    fs::path to_path() const
    {
        if (value == unknown)
        {
            return fs::path();
        }
        return fs::path("/dev") /
               fs::path(nameMatching[static_cast<uint8_t>(value)]);
    }

  private:
    Value value = unknown;

    static const inline std::vector<std::string> nameMatching = {
        "nbd0", "nbd1", "nbd2", "nbd3", "nbd4", "nbd5",
        "nbd6", "nbd7", "nbd8", "nbd9", "nbd10"};
};

enum class StateChange
{
    notMonitored,
    unknown,
    removed,
    inserted
};

class DeviceMonitor
{
  public:
    DeviceMonitor(boost::asio::io_context& ioc) : ioc(ioc), monitorSd(ioc)
    {
        udev = std::unique_ptr<udev::udev, udev::udevDeleter>(udev::udev_new());
        if (!udev)
        {
            throw std::system_error(EFAULT, std::generic_category(),
                                    "Unable to create uDev handler.");
        }

        monitor = std::unique_ptr<udev::udev_monitor, udev::monitorDeleter>(
            udev::udev_monitor_new_from_netlink(udev.get(), "kernel"));

        if (!monitor)
        {
            throw std::system_error(EFAULT, std::generic_category(),
                                    "Unable to create uDev Monitor handler.");
        }
        int rc = udev_monitor_filter_add_match_subsystem_devtype(
            monitor.get(), "block", "disk");

        if (rc)
        {
            throw std::system_error(EFAULT, std::generic_category(),
                                    "Could not apply filters.");
        }
        rc = udev_monitor_enable_receiving(monitor.get());
        if (rc)
        {
            throw std::system_error(EFAULT, std::generic_category(),
                                    "Enable receiving failed.");
        }
        monitorSd.assign(udev_monitor_get_fd(monitor.get()));
    }

    DeviceMonitor(const DeviceMonitor&) = delete;
    DeviceMonitor(DeviceMonitor&&) = delete;

    DeviceMonitor& operator=(const DeviceMonitor&) = delete;
    DeviceMonitor& operator=(DeviceMonitor&&) = delete;

    template <typename DeviceChangeStateCb>
    void run(DeviceChangeStateCb callback)
    {
        boost::asio::spawn(ioc, [this,
                                 callback](boost::asio::yield_context yield) {
            boost::system::error_code ec;
            while (1)
            {
                monitorSd.async_wait(
                    boost::asio::posix::stream_descriptor::wait_read,
                    yield[ec]);

                std::unique_ptr<udev::udev_device, udev::deviceDeleter> device =
                    std::unique_ptr<udev::udev_device, udev::deviceDeleter>(
                        udev::udev_monitor_receive_device(monitor.get()));
                if (device)
                {
                    const char* devAction =
                        udev_device_get_action(device.get());
                    if (devAction == nullptr)
                    {
                        LogMsg(Logger::Error,
                               "[DeviceMonitor]: Received NULL action.");
                        continue;
                    }
                    if (strcmp(devAction, "change") != 0)
                    {
                        continue;
                    }

                    const char* sysname = udev_device_get_sysname(device.get());
                    if (sysname == nullptr)
                    {
                        LogMsg(Logger::Error,
                               "[DeviceMonitor]: Received NULL sysname.");
                        continue;
                    }

                    NBDDevice nbdDevice(sysname);
                    if (!nbdDevice)
                    {
                        continue;
                    }

                    auto monitoredDevice = devices.find(nbdDevice);
                    if (monitoredDevice == devices.cend())
                    {
                        continue;
                    }

                    const char* sizeStr =
                        udev_device_get_sysattr_value(device.get(), "size");
                    if (sizeStr == nullptr)
                    {
                        LogMsg(Logger::Error,
                               "[DeviceMonitor]: Received NULL size.");
                        continue;
                    }

                    uint64_t size = 0;
                    try
                    {
                        size = std::stoul(sizeStr, 0, 0);
                    }
                    catch (const std::exception& e)
                    {
                        LogMsg(Logger::Error,
                               "[DeviceMonitor]: Could not convert "
                               "size "
                               "to integer.");
                        continue;
                    }
                    if (size > 0 &&
                        monitoredDevice->second != StateChange::inserted)
                    {
                        LogMsg(Logger::Info,
                               "[DeviceMonitor]: ", nbdDevice.to_path(),
                               " inserted.");
                        monitoredDevice->second = StateChange::inserted;
                        callback(nbdDevice, StateChange::inserted);
                    }
                    else if (size == 0 &&
                             monitoredDevice->second != StateChange::removed)
                    {
                        LogMsg(Logger::Info,
                               "[DeviceMonitor]: ", nbdDevice.to_path(),
                               " removed.");
                        monitoredDevice->second = StateChange::removed;
                        callback(nbdDevice, StateChange::removed);
                    }
                }
            }
        }, boost::asio::detached);
    }

    void addDevice(const NBDDevice& device)
    {
        LogMsg(Logger::Info, "[DeviceMonitor]: watch on ", device.to_path());
        devices.insert(
            std::pair<NBDDevice, StateChange>(device, StateChange::unknown));
    }

    StateChange getState(const NBDDevice& device)
    {
        auto monitoredDevice = devices.find(device);
        if (monitoredDevice != devices.cend())
        {
            return monitoredDevice->second;
        }
        return StateChange::notMonitored;
    }

  private:
    boost::asio::io_context& ioc;
    boost::asio::posix::stream_descriptor monitorSd;

    std::unique_ptr<udev::udev, udev::udevDeleter> udev;
    std::unique_ptr<udev::udev_monitor, udev::monitorDeleter> monitor;

    boost::container::flat_map<NBDDevice, StateChange> devices;
};

class Process : public std::enable_shared_from_this<Process>
{
  public:
    Process(boost::asio::io_context& ioc, const std::string& name,
            const std::string& app, const NBDDevice& dev) :
        ioc(ioc),
        pipe(ioc), name(name), app(app), dev(dev)
    {}

    bool authFailure = false;
    bool certFailure = false;

    template <typename ExitCb>
    bool spawn(const std::vector<std::string>& args, ExitCb&& onExit)
    {
        std::error_code ec;
        LogMsg(Logger::Info, "[Process]: Spawning ", app, " (", args, ")");
        child = boost::process::v1::child(
            app, boost::process::v1::args(args),
            (boost::process::v1::std_out & boost::process::v1::std_err) > pipe, ec,
            ioc);

        if (ec)
        {
            LogMsg(Logger::Error,
                   "[Process]: Error while creating child process: ", ec);
            return false;
        }

        boost::asio::spawn(
            ioc, [this, self = shared_from_this(),
                  onExit{std::move(onExit)}](boost::asio::yield_context yield) {
                boost::system::error_code bec;
                std::string line;
                boost::asio::dynamic_string_buffer buffer{line};
                LogMsg(Logger::Info,
                       "[Process]: Start reading console from nbd-client");
                while (1)
                {
                    auto x = boost::asio::async_read_until(
                        self->pipe, std::move(buffer), '\n', yield[bec]);
                    auto lineBegin = line.begin();
                    while (lineBegin != line.end())
                    {
                        auto lineEnd = find(lineBegin, line.end(), '\n');
                        std::string lineStr(lineBegin, lineEnd);
                        LogMsg(Logger::Debug, "[Process]: (", self->name, ") ",
                               lineStr);
                        // Detect HTTP 401/403 from the nbdkit curl plugin
                        // output ("returned error: NNN"). This is best-effort;
                        // the substring is nbdkit-specific and may change.
                        if (lineStr.find("returned error: 401") !=
                                std::string::npos ||
                            lineStr.find("returned error: 403") !=
                                std::string::npos)
                        {
                            self->authFailure = true;
                        }
                        // Detect TLS certificate verification failure from the
                        // nbdkit curl plugin. These errors occur before any HTTP
                        // exchange when the server cert cannot be verified
                        // (e.g. CA not installed in the BMC trust store).
                        if (lineStr.find("SSL certificate problem") !=
                                std::string::npos ||
                            lineStr.find("SSL/TLS handshake failed") !=
                                std::string::npos ||
                            lineStr.find("certificate verify failed") !=
                                std::string::npos)
                        {
                            self->certFailure = true;
                        }
                        if (lineEnd == line.end())
                        {
                            break;
                        }
                        lineBegin = lineEnd + 1;
                    }

                    buffer.consume(x);
                    if (bec)
                    {
                        LogMsg(Logger::Debug, "[Process]: (", self->name,
                               ") Loop Error: ", bec);
                        break;
                    }
                }
                LogMsg(Logger::Info, "[Process]: Exiting from COUT Loop");
                // The process shall be dead, or almost here, give it a chance
                LogMsg(Logger::Debug,
                       "[Process]: Waiting process to finish normally");
                boost::asio::steady_timer timer(self->ioc);
                int32_t waitCnt = 20;
                while (self->child.running() && waitCnt > 0)
                {
                    boost::system::error_code ignored_ec;
                    timer.expires_after(std::chrono::milliseconds(100));
                    timer.async_wait(yield[ignored_ec]);
                    waitCnt--;
                }
                if (self->child.running())
                {
                    self->child.terminate();
                }

                self->child.wait();
                LogMsg(Logger::Info, "[Process]: running: ", self->child.running(),
                       " EC: ", self->child.exit_code(),
                       " Native: ", self->child.native_exit_code());

                onExit(self->child.exit_code(), self->dev.isReady(),
                       self->authFailure, self->certFailure);
            }, boost::asio::detached);
        return true;
    }

    void stop()
    {
        auto self = shared_from_this();
        boost::asio::spawn(ioc, [this, self](boost::asio::yield_context yield) {
            // The Good
            self->dev.disconnect();

            // The Ugly (but required)
            boost::asio::steady_timer timer(self->ioc);
            int32_t waitCnt = 20;
            while (self->child.running() && waitCnt > 0)
            {
                boost::system::error_code ignored_ec;
                timer.expires_after(std::chrono::milliseconds(100));
                timer.async_wait(yield[ignored_ec]);
                waitCnt--;
            }
            if (self->child.running())
            {
                LogMsg(Logger::Info, "[Process] Terminate if process doesnt "
                                     "want to exit nicely");
                self->child.terminate();
            }
        }, boost::asio::detached);
    }

    std::string application()
    {
        return app;
    }

  private:
    boost::asio::io_context& ioc;
    boost::process::v1::child child;
    boost::process::v1::async_pipe pipe;
    std::string name;
    std::string app;
    const NBDDevice& dev;
};

class FsHelper
{
  protected:
    static bool echoToFile(const fs::path& fname, const std::string& content)
    {
        std::ofstream fileWriter;
        fileWriter.exceptions(std::ofstream::failbit | std::ofstream::badbit);
        fileWriter.open(fname, std::ios::out | std::ios::app);
        fileWriter << content << std::endl; // Make sure for new line and flush
        fileWriter.close();
        LogMsg(Logger::Debug, "echo ", content, " > ", fname);
        return true;
    }
};

struct UsbGadget : private FsHelper
{
  public:
    static int32_t configure(const std::string& name, const NBDDevice& nbd,
                             StateChange change, const bool rw = false)
    {
        return configure(name, nbd.to_path(), change, rw);
    }

    static int32_t configure(const std::string& name, const fs::path& path,
                             StateChange change, const bool rw = false)
    {
        LogMsg(Logger::Info, "[App]: Configure USB Gadget (name=", name,
               ", path=", path, ", State=", static_cast<uint32_t>(change), ")");
        bool success = true;
        std::error_code ec;
        if (change == StateChange::unknown)
        {
            LogMsg(Logger::Critical,
                   "[App]: Change to unknown state is not possible");
            return -1;
        }

        const fs::path gadgetDir =
            "/sys/kernel/config/usb_gadget/mass-storage-" + name;
        const fs::path funcMassStorageDir =
            gadgetDir / "functions/mass_storage.usb0";
        const fs::path stringsDir = gadgetDir / "strings/0x409";
        const fs::path configDir = gadgetDir / "configs/c.1";
        const fs::path massStorageDir = configDir / "mass_storage.usb0";
        const fs::path configStringsDir = configDir / "strings/0x409";
        const std::regex gadgetDirRegex("gadget.[0-9]");

        if (change == StateChange::inserted)
        {
            try
            {
                fs::create_directories(gadgetDir);
                echoToFile(gadgetDir / "idVendor", "0x1d6b");
                echoToFile(gadgetDir / "idProduct", "0x0104");
                fs::create_directories(stringsDir);
                echoToFile(stringsDir / "manufacturer", "OpenBMC");
                echoToFile(stringsDir / "product", "Virtual Media Device (" + name + ")");
                fs::create_directories(configStringsDir);
                echoToFile(configStringsDir / "configuration", "config 1");
                fs::create_directories(funcMassStorageDir);
                fs::create_directories(funcMassStorageDir / "lun.0");
                fs::create_directory_symlink(funcMassStorageDir,
                                             massStorageDir);
                echoToFile(funcMassStorageDir / "lun.0/removable", "1");
                echoToFile(funcMassStorageDir / "lun.0/ro", rw ? "0" : "1");
                echoToFile(funcMassStorageDir / "lun.0/cdrom", "0");
                echoToFile(funcMassStorageDir / "lun.0/file", path);

                for (const auto& port : fs::directory_iterator(
                         "/sys/bus/platform/devices/1e6a0000.usb-vhub"))
                {
                    if (fs::is_directory(port) && !fs::is_symlink(port))
                    {
                        for (const auto &dir : fs::directory_iterator(port))
                        {
                            if (fs::is_directory(dir) &&
                                 // In the port directory, check for the gadget directory. Ex: gadget.2
                                 std::regex_search(dir.path().filename().string(), gadgetDirRegex) &&
                                 !fs::exists(dir.path() / "suspended"))
                            {
                                const std::string portId = port.path().filename();
                                LogMsg(Logger::Debug, "Use port : ", portId);
                                echoToFile(gadgetDir / "UDC", portId);
                                return 0;
                            }
                        }
                    }
                }
            }
            catch (fs::filesystem_error& e)
            {
                // Got error perform cleanup
                LogMsg(Logger::Error, "[App]: UsbGadget: ", e.what());
                success = false;
            }
            catch (std::ofstream::failure& e)
            {
                // Got error perform cleanup
                LogMsg(Logger::Error, "[App]: UsbGadget: ", e.what());
                success = false;
            }
        }
        // Remove via fs::remove() instead of system() to avoid shell injection.
        const std::array<fs::path, 6> dirsToRemove = {
            massStorageDir, funcMassStorageDir, configStringsDir,
            configDir,      stringsDir,         gadgetDir,
        };

        // Unbind the gadget from the UDC before tearing down the directories.
        try
        {
            echoToFile(gadgetDir / "UDC", "");
        }
        catch (std::ofstream::failure& e)
        {
            LogMsg(Logger::Debug,
                   "[App]: UsbGadget UDC unbind: ", e.what());
        }

        for (const auto& p : dirsToRemove)
        {
            std::error_code removeEc;
            fs::remove(p, removeEc);
            if (removeEc)
            {
                success = false;
                LogMsg(Logger::Error, "[App]: UsbGadget : Can't remove ", p,
                       ": ", removeEc.message());
            }
        }

        if (success)
        {
            return 0;
        }
        return -1;
    }
};

class UdevGadget : private FsHelper
{
  public:
    // Workaround for HSD18020136609: Can not mount image using Virtual media
    // and CIFS protocol
    // This force-triggers udev change events for nbd[0-3] devices, which
    // prevents from disconnection on first mount event after reboot. The actual
    // rootcause is related with kernel changes that occured between 5.10.67 and
    // 5.14.11. This lead will continue to be investigated in order to provide
    // proper fix.
    static void forceUdevChange()
    {
        std::string changeStr = "change";
        echoToFile("/sys/block/nbd0/uevent", changeStr);
        echoToFile("/sys/block/nbd1/uevent", changeStr);
        echoToFile("/sys/block/nbd2/uevent", changeStr);
        echoToFile("/sys/block/nbd3/uevent", changeStr);
    }

    static void setMaxSectorsKb()
    {
        std::string maxSectorsKbStr = "128";
        echoToFile("/sys/block/nbd0/queue/max_sectors_kb", maxSectorsKbStr);
        echoToFile("/sys/block/nbd1/queue/max_sectors_kb", maxSectorsKbStr);
        echoToFile("/sys/block/nbd2/queue/max_sectors_kb", maxSectorsKbStr);
        echoToFile("/sys/block/nbd3/queue/max_sectors_kb", maxSectorsKbStr);
    }
};
