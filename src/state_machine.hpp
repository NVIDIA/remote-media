#pragma once

#include "configuration.hpp"
#include "logger.hpp"
#include "smb.hpp"
#include "system.hpp"
#include "utils.hpp"

#include <sys/mount.h>

#include <phosphor-logging/redfish_event_log.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <variant>

using namespace phosphor::logging;
struct MountPointStateMachine
{
    struct InvalidStateError : std::runtime_error
    {
        InvalidStateError(const char* what) : std::runtime_error(what)
        {}
    };

    struct Error
    {
        std::errc code;
        std::string message;
    };

    struct BasicState
    {
        BasicState(MountPointStateMachine& machine,
                   const char* stateName = nullptr) :
            machine{machine},
            stateName{stateName}
        {
            if (stateName != nullptr)
            {
                LogMsg(Logger::Debug, machine.name, " State changed to ",
                       stateName);
            }
        }

        BasicState(const BasicState& state) :
            machine{state.machine}, stateName{state.stateName}
        {}

        BasicState(const BasicState& state, const char* stateName) :
            machine{state.machine}, stateName{stateName}
        {
            LogMsg(Logger::Debug, machine.name, " State changed to ",
                   stateName);
        }

        BasicState& operator=(BasicState&& state)
        {
            machine = std::move(state.machine);
            stateName = std::move(state.stateName);
            return *this;
        }

        virtual void onEnter(){};

        MountPointStateMachine& machine;
        const char* stateName = nullptr;
    };

    struct InitialState : public BasicState
    {
        InitialState(const BasicState& state) :
            BasicState(state, __FUNCTION__){};
        InitialState(MountPointStateMachine& machine) :
            BasicState(machine, __FUNCTION__){};
    };

    struct ReadyState : public BasicState
    {
        ReadyState(const BasicState& state) : BasicState(state, __FUNCTION__){};

        ReadyState(const BasicState& state, const std::errc& ec,
                   const std::string& message) :
            BasicState(state, __FUNCTION__),
            error{{ec, message}}
        {
            LogMsg(Logger::Error, state.machine.name,
                   " Errno = ", static_cast<int>(ec), " : ", message);
        };

        virtual void onEnter()
        {
            if (machine.target)
            {
                // Cleanup after previously mounted device
                if (machine.target->mountDir)
                {
                    SmbShare::unmount(*machine.target->mountDir);
                }

                machine.target.reset();
            }
        }

        std::optional<Error> error;
    };

    struct ActivatingState : public BasicState
    {
        ActivatingState(const BasicState& state) :
            BasicState(state, __FUNCTION__)
        {}

        virtual void onEnter()
        {
            // Reset previous exit code
            machine.exitCode = -1;

            machine.emitActivationStartedEvent();
        }
    };

    struct WaitingForGadgetState : public BasicState
    {
        WaitingForGadgetState(const BasicState& state) :
            BasicState(state, __FUNCTION__)
        {}

        std::weak_ptr<Process> process;
    };

    struct ActiveState : public BasicState
    {
        ActiveState(const BasicState& state) : BasicState(state, __FUNCTION__)
        {}
        ActiveState(const WaitingForGadgetState& state) :
            BasicState(state, __FUNCTION__), process{state.process} {};

        std::weak_ptr<Process> process;
    };

    struct WaitingForProcessEndState : public BasicState
    {
        WaitingForProcessEndState(const BasicState& state) :
            BasicState(state, __FUNCTION__)
        {}
        WaitingForProcessEndState(const ActiveState& state) :
            BasicState(state, __FUNCTION__), process{state.process}
        {}
        WaitingForProcessEndState(const WaitingForGadgetState& state) :
            BasicState(state, __FUNCTION__), process{state.process}
        {}

        std::weak_ptr<Process> process;
    };

    using State = std::variant<InitialState, ReadyState, ActivatingState,
                               WaitingForGadgetState, ActiveState,
                               WaitingForProcessEndState>;

    struct BasicEvent
    {
        BasicEvent(const char* eventName) : eventName(eventName)
        {}

        inline void transitionError(const char* en, const BasicState& state)
        {
            LogMsg(Logger::Critical, state.machine.name, " Unexpected event ",
                   eventName, " received in ", state.stateName,
                   "state. Review and correct state transisions.");
        }
        virtual State operator()(const InitialState& state)
        {
            transitionError(eventName, state);
            return state;
        }
        virtual State operator()(const ReadyState& state)
        {
            transitionError(eventName, state);
            return state;
        }
        virtual State operator()(const ActivatingState& state)
        {
            transitionError(eventName, state);
            return state;
        }
        virtual State operator()(const WaitingForGadgetState& state)
        {
            transitionError(eventName, state);
            return state;
        }
        virtual State operator()(const ActiveState& state)
        {
            transitionError(eventName, state);
            return state;
        }
        virtual State operator()(const WaitingForProcessEndState& state)
        {
            transitionError(eventName, state);
            return state;
        }
        const char* eventName;
    };

    struct RegisterDbusEvent : public BasicEvent
    {
        RegisterDbusEvent(
            std::shared_ptr<sdbusplus::asio::connection> bus,
            std::shared_ptr<sdbusplus::asio::object_server> objServer) :
            BasicEvent(__FUNCTION__),
            bus(bus), objServer(objServer),
            emitMountEvent(std::move(emitMountEvent))
        {}

        State operator()(const InitialState& state)
        {
            const bool isLegacy =
                (state.machine.config.mode == Configuration::Mode::legacy);
            addMountPointInterface(state);
            addProcessInterface(state);
            addServiceInterface(state, isLegacy);
            // Workaround for HSD18020136609. Details in system.hpp.
            UdevGadget::forceUdevChange();
            return ReadyState(state);
        }

        template <typename AnyState>
        State operator()(const AnyState& state)
        {
            LogMsg(Logger::Critical, state.machine.name,
                   " If you receiving this error, this means "
                   "your FSM is broken. Rethink!");
            return InitialState(state);
        }

      private:
        std::string getObjectPath(const MountPointStateMachine& machine)
        {
            LogMsg(Logger::Debug, "getObjectPath entry()");
            return machine.getObjectPath();
        }

        std::string getObjectPath(const InitialState& state)
        {
            return getObjectPath(state.machine);
        }

        void addProcessInterface(const InitialState& state)
        {
            std::string objPath = getObjectPath(state);

            auto processIface = objServer->add_interface(
                objPath + state.machine.name,
                "xyz.openbmc_project.VirtualMedia.Process");

            processIface->register_property(
                "Active", bool(false),
                [](const bool& req, bool& property) { return 0; },
                [&machine = state.machine](const bool& property) {
                    if (std::get_if<ActiveState>(&machine.state))
                    {
                        return true;
                    }
                    else
                    {
                        return false;
                    }
                });
            processIface->register_property(
                "ExitCode", int32_t(0),
                [](const int32_t& req, int32_t& property) { return 0; },
                [&machine = state.machine](const int32_t& property) {
                    return machine.exitCode;
                });
            processIface->register_property(
                "CDInstance", int32_t(2),
                [](const int32_t& req, int32_t& property) {
                    property = req;
                    return req;
                },
                [](int32_t& property) -> int32_t { return property; });
            processIface->initialize();
        }

        void addMountPointInterface(const InitialState& state)
        {
            std::string objPath = getObjectPath(state);

            auto iface = objServer->add_interface(
                objPath + state.machine.name,
                "xyz.openbmc_project.VirtualMedia.MountPoint");
            iface->register_property(
                "Device", state.machine.config.nbdDevice.to_string());
            iface->register_property("EndpointId",
                                     state.machine.config.endPointId);
            iface->register_property("Socket", state.machine.config.unixSocket);

            iface->register_property(
                "ImageURL", std::string(""),
                [](const std::string& req, std::string& property) {
                    property = req;
                    return -1;
                },
                [&machine = state.machine](const std::string& property) {
                    if (std::get_if<ActiveState>(&machine.state))
                    {
                        return std::string(machine.target->imgUrl);
                    }
                    else
                    {
                        return std::string("");
                    }
                });
            iface->register_property(
                "User", std::string(""),
                [](const std::string& req, std::string& property) {
                    property = req;
                    return -1;
                },
                [&machine = state.machine](const std::string& property) {
                    if (std::get_if<ActiveState>(&machine.state))
                    {
                        return USER;
                    }
                    else
                    {
                        return std::string("");
                    }
                });
            iface->register_property(
                "WriteProtected", bool(true),
                [](const bool& req, bool& property) { return 0; },
                [&machine = state.machine](const bool& property) {
                    if (!machine.target->rw)
                    {
                        return true;
                    }
                    else
                    {
                        return false;
                    }
                });

            iface->initialize();
        }

        void addServiceInterface(const InitialState& state, const bool isLegacy)
        {
            const std::string name = "xyz.openbmc_project.VirtualMedia." +
                                     std::string(isLegacy ? "Legacy" : "Proxy");

            const std::string path = getObjectPath(state) + state.machine.name;

            auto iface = objServer->add_interface(path, name);

            // Common unmount
            iface->register_method(
                "Unmount",
                [&machine = state.machine](boost::asio::yield_context yield) {
                    LogMsg(Logger::Info, "[App]: Unmount called on ",
                           machine.name);
                    try
                    {
                        machine.emitUnmountEvent();
                    }
                    catch (InvalidStateError& e)
                    {
                        throw sdbusplus::exception::SdBusError(EPERM, e.what());
                        return false;
                    }

                    boost::asio::steady_timer timer(machine.ioc.get());
                    int waitCnt = 120;
                    while (waitCnt > 0)
                    {
                        if (std::get_if<ReadyState>(&machine.state))
                        {
                            break;
                        }
                        boost::system::error_code ignored_ec;
                        timer.expires_from_now(std::chrono::milliseconds(100));
                        timer.async_wait(yield[ignored_ec]);
                        waitCnt--;
                    }
                    return true;
                });

            // Common mount
            const auto handleMount = [](boost::asio::yield_context yield,
                                        MountPointStateMachine& machine) {
                try
                {
                    machine.emitMountEvent();
                }
                catch (InvalidStateError& e)
                {
                    throw sdbusplus::exception::SdBusError(EPERM, e.what());
                    return false;
                }

                boost::asio::steady_timer timer(machine.ioc.get());
                int waitCnt = 120;
                while (waitCnt > 0)
                {
                    if (auto s = std::get_if<ReadyState>(&machine.state))
                    {
                        if (s->error)
                        {
                            throw sdbusplus::exception::SdBusError(
                                static_cast<int>(s->error->code),
                                s->error->message.c_str());
                        }
                        return false;
                    }
                    if (std::get_if<ActiveState>(&machine.state))
                    {
                        return true;
                    }
                    boost::system::error_code ignored_ec;
                    timer.expires_from_now(std::chrono::milliseconds(100));
                    timer.async_wait(yield[ignored_ec]);
                    waitCnt--;
                }
                return false;
            };

            // Mount specialization
            if (isLegacy)
            {
                using sdbusplus::message::unix_fd;
                using optional_fd = std::variant<int, unix_fd>;

                iface->register_method(
                    "Mount", [&machine = state.machine, this, handleMount](
                                 boost::asio::yield_context yield,
                                 std::string imgUrl, bool rw, optional_fd fd) {
                        LogMsg(Logger::Info, "[App]: Mount called on ",
                               getObjectPath(machine), machine.name);

                        machine.target = {imgUrl, rw};

                        if (std::holds_alternative<unix_fd>(fd))
                        {
                            LogMsg(Logger::Debug, "[App] Extra data available");

                            // Open pipe and prepare output buffer
                            boost::asio::posix::stream_descriptor secretPipe(
                                machine.ioc.get(),
                                dup(std::get<unix_fd>(fd).fd));
                            std::array<char, utils::secretLimit> buf;

                            // Read data
                            auto size = secretPipe.async_read_some(
                                boost::asio::buffer(buf), yield);

                            // Validate number of NULL delimiters, ensures
                            // further operations are safe
                            auto nullCount = std::count(
                                buf.begin(), buf.begin() + size, '\0');
                            if (nullCount != 2)
                            {
                                throw sdbusplus::exception::SdBusError(
                                    EINVAL, "Malformed extra data");
                            }

                            // First 'part' of payload
                            std::string user(buf.begin());
                            // Second 'part', after NULL delimiter
                            std::string pass(buf.begin() + user.length() + 1);

                            // Encapsulate credentials into safe buffer
                            machine.target->credentials =
                                std::make_unique<utils::CredentialsProvider>(
                                    std::move(user), std::move(pass));

                            // Cover the tracks
                            utils::secureCleanup(buf);
                        }

                        try
                        {
                            auto ret = handleMount(yield, machine);
                            machine.target->credentials.reset();
                            return ret;
                        }
                        catch (...)
                        {
                            machine.target->credentials.reset();
                            throw;
                            return false;
                        }
                    });
            }
            else
            {
                iface->register_method(
                    "Mount", [&machine = state.machine, this,
                              handleMount](boost::asio::yield_context yield) {
                        LogMsg(Logger::Info, "[App]: Mount called on ",
                               getObjectPath(machine), machine.name);

                        return handleMount(yield, machine);
                    });
            }

            iface->initialize();
        }

        std::shared_ptr<sdbusplus::asio::connection> bus;
        std::shared_ptr<sdbusplus::asio::object_server> objServer;
        std::function<void(void)> emitMountEvent;
    };

    struct MountEvent : public BasicEvent
    {
        MountEvent() : BasicEvent(__FUNCTION__)
        {}
        State operator()(const ReadyState& state)
        {
            return ActivatingState(state);
        }

        template <typename AnyState>
        State operator()(const AnyState& state)
        {
            throw InvalidStateError("Could not mount on not empty slot");
        }
    };

    struct UnmountEvent : public BasicEvent
    {
        UnmountEvent() : BasicEvent(__FUNCTION__)
        {}
        State operator()(const ActivatingState& state)
        {
            return ReadyState(state);
        }
        State operator()(const WaitingForGadgetState& state)
        {
            state.machine.stopProcess(state.process);
            return WaitingForProcessEndState(state);
        }
        State operator()(const ActiveState& state)
        {
            if (!state.machine.removeUsbGadget(state))
            {
                return ReadyState(state, std::errc::device_or_resource_busy,
                                  "Unable to unmount gadget");
            }
            state.machine.stopProcess(state.process);
            // send an event
            auto dbusObjectPath = state.machine.getObjectPath() + state.machine.name;
            sendEvent(state.machine.bus, MESSAGE_TYPE::RESOURCE_DELETED,
                  Entry::Level::Informational, std::vector<std::string>{}, dbusObjectPath);
            return WaitingForProcessEndState(state);
        }
        State operator()(const WaitingForProcessEndState& state)
        {
            throw InvalidStateError("Could not unmount on empty slot");
        }
        State operator()(const ReadyState& state)
        {
            throw InvalidStateError("Could not unmount on empty slot");
        }
    };

    struct SubprocessStoppedEvent : public BasicEvent
    {
        SubprocessStoppedEvent() : BasicEvent(__FUNCTION__)
        {}
        State operator()(const ActivatingState& state)
        {
            return ReadyState(state);
        }
        State operator()(const WaitingForGadgetState& state)
        {
            state.machine.stopProcess(state.process);
            return ReadyState(state, std::errc::io_error,
                              "Process ended prematurely");
        }
        State operator()(const ActiveState& state)
        {
            if (!state.machine.removeUsbGadget(state))
            {
                return ReadyState(state, std::errc::device_or_resource_busy,
                                  "Unable to unmount gadget");
            }
            return ReadyState(state);
        }
        State operator()(const WaitingForProcessEndState& state)
        {
            return ReadyState(state);
        }
    };

    struct ActivationStartedEvent : public BasicEvent
    {
        ActivationStartedEvent() : BasicEvent(__FUNCTION__)
        {}
        State operator()(const ActivatingState& state)
        {
            if (state.machine.config.mode == Configuration::Mode::proxy)
            {
                return activateProxyMode(state);
            }
            return activateLegacyMode(state);
        }

        State activateProxyMode(const ActivatingState& state)
        {
            auto process = std::make_shared<Process>(
                state.machine.ioc.get(), state.machine.name,
                "/usr/sbin/nbd-client", state.machine.config.nbdDevice);
            if (!process)
            {
                return ReadyState(state, std::errc::operation_canceled,
                                  "Failed to allocate process");
            }
            if (!process->spawn(
                    Configuration::MountPoint::toArgs(state.machine.config),
                    [&machine = state.machine](int exitCode, bool isReady) {
                        LogMsg(Logger::Info, machine.name, " process ended.");
                        machine.exitCode = exitCode;
                        machine.emitSubprocessStoppedEvent();
                    }))
            {
                return ReadyState(state, std::errc::operation_canceled,
                                  "Failed to spawn process");
            }
            auto newState = WaitingForGadgetState(state);
            newState.process = process;
            return newState;
        }

        State activateLegacyMode(const ActivatingState& state)
        {
            LogMsg(
                Logger::Debug, state.machine.name,
                " Mount requested on address: ", state.machine.target->imgUrl,
                " ; RW: ", state.machine.target->rw);

            if (isCifsUrl(state.machine.target->imgUrl))
            {
                return mountSmbShare(state);
            }
            else if (isHttpsUrl(state.machine.target->imgUrl))
            {
                return mountHttpsShare(state);
            }

            return ReadyState(state, std::errc::invalid_argument,
                              "URL not recognized");
        }

        State mountSmbShare(const ActivatingState& state)
        {
            auto mountDir = SmbShare::createMountDir(state.machine.name);
            if (!mountDir)
            {
                return ReadyState(state, std::errc::io_error,
                                  "Failed to create mount directory");
            }

            SmbShare smb(*mountDir);
            fs::path remote = getImagePath(state.machine.target->imgUrl);
            auto remoteParent = "/" + remote.parent_path().string();
            auto localFile = *mountDir / remote.filename();

            LogMsg(Logger::Debug, state.machine.name, " Remote name: ", remote,
                   "\n Remote parent: ", remoteParent,
                   "\n Local file: ", localFile);

            if (!smb.mount(remoteParent, state.machine.target->rw,
                           state.machine.target->credentials))
            {
                fs::remove_all(*mountDir);
                return ReadyState(state, std::errc::invalid_argument,
                                  "Failed to mount CIFS share");
            }

            auto process = spawnNbdKit(state.machine, localFile);
            if (!process)
            {
                SmbShare::unmount(*mountDir);
                return ReadyState(state, std::errc::operation_canceled,
                                  "Unable to setup NbdKit");
            }

            auto newState = WaitingForGadgetState(state);
            newState.process = process;
            newState.machine.target->mountDir = *mountDir;

            return newState;
        }

        State mountHttpsShare(const ActivatingState& state)
        {
            auto& machine = state.machine;

            auto process = spawnNbdKit(machine, machine.target->imgUrl);
            if (!process)
            {
                return ReadyState(state, std::errc::invalid_argument,
                                  "Failed to mount HTTPS share");
            }

            auto newState = WaitingForGadgetState(state);
            newState.process = process;
            return newState;
        }

        static std::shared_ptr<Process>
            spawnNbdKit(MountPointStateMachine& machine,
                        std::unique_ptr<utils::VolatileFile>&& secret,
                        const std::vector<std::string>& params)
        {
            // Investigate
            auto process = std::make_shared<Process>(
                machine.ioc.get(), machine.name, "/usr/sbin/nbdkit",
                machine.config.nbdDevice);
            if (!process)
            {
                LogMsg(Logger::Error, machine.name,
                       " Failed to create Process for: ", machine.name);
                return {};
            }

            // Cleanup of previous socket
            if (fs::exists(machine.config.unixSocket))
            {
                LogMsg(Logger::Debug, machine.name,
                       " Removing previously mounted socket: ",
                       machine.config.unixSocket);
                if (!fs::remove(machine.config.unixSocket))
                {
                    LogMsg(Logger::Error, machine.name,
                           " Unable to remove pre-existing socket :",
                           machine.config.unixSocket);
                    return {};
                }
            }

            std::string nbd_client =
                "/usr/sbin/nbd-client " +
                boost::algorithm::join(
                    Configuration::MountPoint::toArgs(machine.config), " ");

            std::vector<std::string> args = {
                // Listen for client on this unix socket...
                "--unix",
                machine.config.unixSocket,

                // ... then connect nbd-client to served image
                "--run",
                nbd_client,

#if VM_VERBOSE_NBDKIT_LOGS
                "--verbose", // swarm of debug logs - only for brave souls
#endif
            };

            if (!machine.target->rw)
            {
                args.push_back("--readonly");
            }

            // Insert extra params
            args.insert(args.end(), params.begin(), params.end());

            if (!process->spawn(
                    args, [&machine = machine, secret = std::move(secret)](
                              int exitCode, bool isReady) {
                        LogMsg(Logger::Info, machine.name, " process ended.");
                        machine.exitCode = exitCode;
                        machine.emitSubprocessStoppedEvent();
                    }))
            {
                LogMsg(Logger::Error, machine.name,
                       " Failed to spawn Process for: ", machine.name);
                return {};
            }

            return process;
        }

        static std::shared_ptr<Process>
            spawnNbdKit(MountPointStateMachine& machine, const fs::path& file)
        {
            return spawnNbdKit(machine, {},
                               {// Use file plugin ...
                                "file",
                                // ... to mount file at this location
                                "file=" + file.string()});
        }

        static std::shared_ptr<Process>
            spawnNbdKit(MountPointStateMachine& machine, const std::string& url)
        {
            std::unique_ptr<utils::VolatileFile> secret;
            std::vector<std::string> params = {
                // Use curl plugin ...
                "curl", "sslverify=false",
                // ... to mount http resource at url
                "url=" + url};

            // Authenticate if needed
            if (machine.target->credentials)
            {
                // Pack password into buffer
                utils::CredentialsProvider::SecureBuffer buff =
                    machine.target->credentials->pack(
                        [](const std::string& user, const std::string& pass,
                           std::vector<char>& buff) {
                            std::copy(pass.begin(), pass.end(),
                                      std::back_inserter(buff));
                        });

                // Prepare file to provide the password with
                secret = std::make_unique<utils::VolatileFile>(std::move(buff));

                params.push_back("user=" + machine.target->credentials->user());
                params.push_back("password=+" + secret->path());
            }

            return spawnNbdKit(machine, std::move(secret), params);
        }

        bool checkUrl(const std::string& urlScheme, const std::string& imageUrl)
        {
            return (urlScheme.compare(imageUrl.substr(0, urlScheme.size())) ==
                    0);
        }

        bool getImagePathFromUrl(const std::string& urlScheme,
                                 const std::string& imageUrl,
                                 std::string* imagePath)
        {
            if (checkUrl(urlScheme, imageUrl))
            {
                if (imagePath != nullptr)
                {
                    *imagePath = imageUrl.substr(urlScheme.size() - 1);
                    return true;
                }
                else
                {
                    LogMsg(Logger::Error, "Invalid parameter provied");
                    return false;
                }
            }
            else
            {
                LogMsg(Logger::Error, "Provied url does not match scheme");
                return false;
            }
        }

        bool isHttpsUrl(const std::string& imageUrl)
        {
            return checkUrl("https://", imageUrl);
        }

        bool getImagePathFromHttpsUrl(const std::string& imageUrl,
                                      std::string* imagePath)
        {
            return getImagePathFromUrl("https://", imageUrl, imagePath);
        }

        bool isCifsUrl(const std::string& imageUrl)
        {
            return checkUrl("smb://", imageUrl);
        }

        bool getImagePathFromCifsUrl(const std::string& imageUrl,
                                     std::string* imagePath)
        {
            return getImagePathFromUrl("smb://", imageUrl, imagePath);
        }

        fs::path getImagePath(const std::string& imageUrl)
        {
            std::string imagePath;

            if (getImagePathFromHttpsUrl(imageUrl, &imagePath))
            {
                return fs::path(imagePath);
            }
            else if (getImagePathFromCifsUrl(imageUrl, &imagePath))
            {
                return fs::path(imagePath);
            }
            else
            {
                LogMsg(Logger::Error, "Unrecognized url's scheme encountered");
                return fs::path("");
            }
        }
    };

    struct UdevStateChangeEvent : public BasicEvent
    {
        UdevStateChangeEvent(const StateChange& devState) :
            BasicEvent(__FUNCTION__), devState{devState}
        {}
        State operator()(const WaitingForGadgetState& state)
        {
            if (devState == StateChange::inserted)
            {
                int32_t ret = UsbGadget::configure(
                    state.machine.name, state.machine.config.nbdDevice,
                    devState,
                    state.machine.target ? state.machine.target->rw : false);
                if (ret == 0)
                {
                    // send an event
                    auto dbusObjectPath = state.machine.getObjectPath() + state.machine.name;
                    sendEvent(state.machine.bus, MESSAGE_TYPE::RESOURCE_CREATED,
                            Entry::Level::Informational, std::vector<std::string>{}, dbusObjectPath);
                    return ActiveState(state);
                }
                return ReadyState(state, std::errc::device_or_resource_busy,
                                  "Unable to configure gadget");
            }
            return ReadyState(state, std::errc::operation_not_supported,
                              "Unexpected udev event: " +
                                  static_cast<int>(devState));
        }

        State operator()(const ReadyState& state)
        {
            if (devState == StateChange::removed)
            {
                LogMsg(Logger::Debug, state.machine.name,
                       " This is acceptable since udev notification is often "
                       "after process is being killed");
            }
            return state;
        }

        template <typename AnyState>
        State operator()(const AnyState& state)
        {
            LogMsg(Logger::Info, name,
                   " Udev State: ", static_cast<int>(devState));
            LogMsg(Logger::Critical, name,
                   " If you receiving this error, this means "
                   "your FSM is broken. Rethink!");
            return state;
        }
        StateChange devState;
    };

    // Helper functions
    bool removeUsbGadget(const BasicState& state)
    {
        int32_t ret = UsbGadget::configure(state.machine.name,
                                           state.machine.config.nbdDevice,
                                           StateChange::removed);
        if (ret != 0)
        {
            // This shouldn't ever happen, perhaps best is to restart app
            LogMsg(Logger::Critical, name, " Some serious failrue happen!");
            return false;
        }
        return true;
    }
    void stopProcess(std::weak_ptr<Process> process)
    {
        if (auto ptr = process.lock())
        {
            ptr->stop();
            return;
        }
        LogMsg(Logger::Info, name, " No process to stop");
    }

    MountPointStateMachine(boost::asio::io_context& ioc,
                           DeviceMonitor& devMonitor, const std::string& name,
                           const Configuration::MountPoint& config,
                           std::shared_ptr<sdbusplus::asio::connection>& bus) :
        ioc{ioc},
        name{name}, config{config}, state{InitialState(*this)}, exitCode{-1}, bus(bus)
    {
        devMonitor.addDevice(config.nbdDevice);
    }

    MountPointStateMachine& operator=(MountPointStateMachine&& machine)
    {
        if (this != &machine)
        {
            state = std::move(machine.state);
            name = std::move(machine.name);
            ioc = machine.ioc;
            config = std::move(machine.config);
            target = std::move(machine.target);
        }
        return *this;
    }

    void emitEvent(BasicEvent&& event)
    {
        std::string stateName = std::visit(
            [](const BasicState& state) { return state.stateName; }, state);

        LogMsg(Logger::Debug, name, " received ", event.eventName, " while in ",
               stateName);

        state = std::visit(event, state);
        std::visit([](BasicState& state) { state.onEnter(); }, state);
    }

    void emitRegisterDBusEvent(
        std::shared_ptr<sdbusplus::asio::object_server> objServer)
    {
        emitEvent(RegisterDbusEvent(bus, objServer));
    }

    void emitMountEvent()
    {
        emitEvent(MountEvent());
    }

    void emitUnmountEvent()
    {
        emitEvent(UnmountEvent());
    }

    void emitActivationStartedEvent()
    {
        emitEvent(ActivationStartedEvent());
    }

    void emitSubprocessStoppedEvent()
    {
        emitEvent(SubprocessStoppedEvent());
    }

    void emitUdevStateChangeEvent(const NBDDevice& dev, StateChange devState)
    {
        if (config.nbdDevice == dev)
        {
            emitEvent(UdevStateChangeEvent(devState));
        }
        else
        {
            LogMsg(Logger::Debug, name, " Ignoring request.");
        }
    }

    struct Target
    {
        std::string imgUrl;
        bool rw;
        std::optional<fs::path> mountDir;
        std::unique_ptr<utils::CredentialsProvider> credentials;
    };

    std::reference_wrapper<boost::asio::io_context> ioc;
    std::string name;
    Configuration::MountPoint config;

    std::optional<Target> target;
    State state;
    int exitCode;
    const std::string proxyObjectPath = "/xyz/openbmc_project/VirtualMedia/Proxy/";
    const std::string legacyObjectPath = "/xyz/openbmc_project/VirtualMedia/Legacy/";
    std::shared_ptr<sdbusplus::asio::connection>& bus;
    std::string getObjectPath() const
    {
        if (config.mode == Configuration::Mode::proxy)
        {
            return proxyObjectPath;
        }
        else
        {
            return legacyObjectPath;
        }
    }
};
