#include "configuration.hpp"
#include "logger.hpp"
#include "persistence.hpp"
#include "state_machine.hpp"
#include "system.hpp"

#include <sys/mount.h>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <variant>
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

        // Persistence hook #1: post-registration startup replay.
        //
        // All slots are now in ReadyState with their D-Bus interfaces
        // published. Walk the durable state directory and, for each saved
        // mount that maps to a slot we just registered, drive a synthetic
        // Mount through the FSM. The replay is posted onto the io_context
        // (rather than invoked inline) so the subprocess spawn that
        // ActivationStartedEvent triggers happens on the running event
        // loop in main() — App's constructor stays non-blocking.
        boost::asio::post(this->ioc,
                          [this]() { this->replayPersistedMounts(); });
    }

  private:
    // Drive a saved mount through the FSM. Runs once at service start for
    // every entry returned by persistence::enumerateSavedSlots().
    //
    // Only genuinely stale entries are cleared here:
    //   * Slot no longer exists in virtual-media.json (config shrunk
    //     between reboots).
    //   * Slot lost its AllowedLocalMountPoints opt-in (operator removed
    //     the durable-platform-image-slot designation). This is the single
    //     eligibility signal; without it we stop replaying and erase the
    //     stale durable file.
    //   * Slot is not in legacy mode.
    //   * driveReplayMount(): the image is outside AllowedLocalMountPoints.
    //
    // Everything else is handed to driveReplayMount(), which is NON-destructive:
    // a missing file:// backing image (its filesystem may be mounted by a later
    // unit) or a remote source that is briefly unreachable at startup is retried
    // with backoff, and if it never succeeds the descriptor is RETAINED for a
    // future boot rather than cleared.
    void replayPersistedMounts()
    {
        const auto saved = persistence::enumerateSavedSlots();
        if (saved.empty())
        {
            return;
        }
        LogMsg(Logger::Info, "[persistence] replaying ", saved.size(),
               " saved mount(s)");

        for (const auto& sm : saved)
        {
            auto it = mpsm.find(sm.slot);
            if (it == mpsm.end())
            {
                LogMsg(Logger::Error,
                       "[persistence] saved slot '", sm.slot,
                       "' is not in current config; clearing stale state");
                persistence::clearState(sm.slot);
                continue;
            }

            auto& machine = *it->second;

            // AllowedLocalMountPoints is the per-slot
            // opt-in. If a previous boot persisted state for this slot
            // but the operator has since stripped its allow-list (or
            // virtual-media.json no longer marks the slot as a durable
            // platform image slot), we must NOT replay — the FSM would
            // reject the mount at isLocalMountPointAllowed() anyway and
            // we'd just sit on a stale state file forever.
            if (machine.config.allowedLocalMountPoints.empty())
            {
                LogMsg(Logger::Error,
                       "[persistence] slot '", sm.slot,
                       "' lost its AllowedLocalMountPoints opt-in; "
                       "clearing stale state instead of replaying");
                persistence::clearState(sm.slot);
                continue;
            }

            // Only legacy slots serve a server-side image. Replaying onto a
            // proxy slot would start an nbd-client listener for a saved URL.
            if (machine.config.mode != Configuration::Mode::legacy)
            {
                LogMsg(Logger::Error,
                       "[persistence] slot '", sm.slot,
                       "' is not a legacy mount point; clearing stale state");
                persistence::clearState(sm.slot);
                continue;
            }

            // NOTE: we intentionally do NOT stat the local file here and clear
            // state if it is missing. The backing filesystem for a file://
            // image can be mounted *after* virtual-media starts (e.g. the BF
            // ARM OS "host-settings" partition that provides /media/bf_arm_os
            // is mounted by a later unit), so a missing file at this instant is
            // usually a boot-time race, not a deleted image. Clearing here was
            // destructive: it permanently wiped a valid persisted mount on
            // every reboot that lost the race. Instead we hand every eligible
            // slot to driveReplayMount(), which waits/retries for the file to
            // appear and, if it never does, RETAINS the descriptor for a future
            // boot rather than deleting it.
            //
            // Drive the saved mount through the normal Activating -> Active
            // path on a coroutine so we can poll the FSM for the outcome and
            // retry transient failures (e.g. a not-yet-mounted local
            // filesystem).
            // Capture sm by value: 'saved' is destroyed when this function
            // returns but the coroutine runs later on the event loop.
            boost::asio::spawn(
                ioc, [this, &machine, sm](boost::asio::yield_context yield) {
                    driveReplayMount(machine, sm, yield);
                });
        }
    }

    // Retry policy for the startup replay. Only file:// mounts are persisted,
    // and such an image can live on a filesystem that is mounted after
    // virtual-media starts (e.g. the BF ARM OS host-settings partition). That
    // is a transient boot-time race, so we retry with a fixed backoff before
    // giving up -- and even then we never delete the descriptor.
    static constexpr int kReplayMaxAttempts = 5;
    static constexpr std::chrono::seconds kReplayRetryDelay{5};
    // Per-attempt settle budget mirrors the D-Bus Mount handler: poll the FSM
    // 120 times every 100ms (~12s) for it to reach Active or fall back to
    // Ready.
    static constexpr int kReplaySettlePolls = 120;
    static constexpr std::chrono::milliseconds kReplayPollInterval{100};

    // Seed the FSM target, fire a synthetic MountEvent, and wait for the
    // machine to settle. Reaches ActiveState on success. Failures (including a
    // local image whose backing filesystem is not mounted yet) are retried up
    // to kReplayMaxAttempts times; if every attempt fails the saved state file
    // is RETAINED so a later boot (or a manual remount) can still recover it.
    // A path rejected by AllowedLocalMountPoints clears the state file.
    void driveReplayMount(MountPointStateMachine& machine,
                          persistence::SavedMount sm,
                          boost::asio::yield_context yield)
    {
        const int maxAttempts = kReplayMaxAttempts;
        boost::asio::steady_timer timer(ioc);

        for (int attempt = 1; attempt <= maxAttempts; ++attempt)
        {
            // A MountEvent is only valid from ReadyState. On the first attempt
            // the freshly-registered slot is in ReadyState; on retries a prior
            // failed attempt should have returned there too. If it is stuck
            // elsewhere (e.g. a timed-out activation still tearing down), stop
            // rather than let emitMountEvent throw.
            if (!std::get_if<MountPointStateMachine::ReadyState>(
                    &machine.state))
            {
                LogMsg(Logger::Error, "[persistence] slot '", sm.slot,
                       "' not ready to (re)mount; leaving state for next boot");
                return;
            }

            // For a local file, wait for the backing filesystem instead of
            // spawning nbdkit on a missing path: the image may live on a
            // partition mounted after us. If it is not there yet, back off and
            // retry rather than failing (and never clear the descriptor).
            if (utils::isFileUrl(sm.imageUrl))
            {
                // Same parser as the mount path, so exists() checks the
                // path that open() will later use.
                std::string localPath;
                MountPointStateMachine::ActivationStartedEvent::
                    getImagePathFromFileUrl(sm.imageUrl, &localPath);
                std::error_code ec;
                if (!std::filesystem::exists(localPath, ec) || ec)
                {
                    LogMsg(Logger::Info, "[persistence] slot '", sm.slot,
                           "' backing image not present yet (", sm.imageUrl,
                           ") on attempt ", attempt, "/", maxAttempts);
                    if (attempt < maxAttempts)
                    {
                        boost::system::error_code ignored;
                        timer.expires_after(kReplayRetryDelay);
                        timer.async_wait(yield[ignored]);
                    }
                    continue;
                }
            }

            // Re-seed the target every attempt: entering ReadyState after a
            // failure resets machine.target in ReadyState::onEnter.
            machine.target = MountPointStateMachine::Target{
                sm.imageUrl, sm.rw, std::nullopt, nullptr};

            try
            {
                LogMsg(Logger::Info, "[persistence] replaying mount for slot '",
                       sm.slot, "' from ", sm.imageUrl, " (attempt ", attempt,
                       "/", maxAttempts, ")");
                machine.emitMountEvent();
            }
            catch (const MountPointStateMachine::InvalidStateError& e)
            {
                LogMsg(Logger::Error, "[persistence] slot '", sm.slot,
                       "' rejected MountEvent: ", e.what(),
                       "; leaving state for next boot");
                machine.target.reset();
                return;
            }

            // Poll until the FSM settles: ActiveState = success, ReadyState =
            // terminal (mount failed and unwound). Anything else means it is
            // still activating; keep waiting up to the budget.
            bool active = false;
            bool backToReady = false;
            for (int poll = 0; poll < kReplaySettlePolls; ++poll)
            {
                if (std::get_if<MountPointStateMachine::ActiveState>(
                        &machine.state))
                {
                    active = true;
                    break;
                }
                if (std::get_if<MountPointStateMachine::ReadyState>(
                        &machine.state))
                {
                    backToReady = true;
                    break;
                }
                boost::system::error_code ignored;
                timer.expires_after(kReplayPollInterval);
                timer.async_wait(yield[ignored]);
            }

            if (active)
            {
                LogMsg(Logger::Info, "[persistence] slot '", sm.slot,
                       "' mounted from ", sm.imageUrl);
                return;
            }

            // An allow-list rejection fails the same way on every retry and
            // every boot, so drop the saved mount instead of retrying.
            if (const auto* ready =
                    std::get_if<MountPointStateMachine::ReadyState>(
                        &machine.state);
                backToReady && ready && ready->error &&
                ready->error->message ==
                    MountPointStateMachine::localMountNotAllowedError)
            {
                LogMsg(Logger::Error, "[persistence] slot '", sm.slot,
                       "' image ", sm.imageUrl,
                       " is not under AllowedLocalMountPoints; clearing state");
                persistence::clearState(sm.slot);
                return;
            }

            LogMsg(Logger::Error, "[persistence] slot '", sm.slot,
                   "' failed to mount from ", sm.imageUrl, " on attempt ",
                   attempt, "/", maxAttempts,
                   backToReady ? " (mount error)" : " (timed out activating)");

            if (attempt < maxAttempts)
            {
                boost::system::error_code ignored;
                timer.expires_after(kReplayRetryDelay);
                timer.async_wait(yield[ignored]);
            }
        }

        LogMsg(Logger::Error, "[persistence] giving up replay for slot '",
               sm.slot, "' after ", maxAttempts,
               " attempt(s); state file retained for a future boot");
    }

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

/*check if the unix socket path end with .sock and the path is in the runtime directory
  or if no directory is provided, add the runtime directory
*/
bool validateSocketPath(Configuration& config)
{
    const char* runtimeDir = std::getenv("RUNTIME_DIRECTORY");
    if(!(runtimeDir && runtimeDir[0])) 
    {
        LogMsg(Logger::Error, "RUNTIME_DIRECTORY is not set");
        return false;
    }
    
    for (auto& [name, entry] : config.mountPoints)
    {
        LogMsg(Logger::Info, "Unix socket: ", entry.unixSocket);
        if (!entry.unixSocket.ends_with(".sock"))
        {
            LogMsg(Logger::Error, "Unix socket ", entry.unixSocket, " must end with .sock");
            return false;
        }
       std::filesystem::path path = entry.unixSocket;
       std::filesystem::path parentPath = path.parent_path();
       if (parentPath.empty())
       {
           entry.unixSocket = std::string(runtimeDir) + "/" + entry.unixSocket;
       }
       else if (parentPath != runtimeDir)
       {
            LogMsg(Logger::Error, "Unix socket ", entry.unixSocket, "dir must be ", runtimeDir);
            return false;
       }
    }
    
    return true;
}

int main()
{
    Configuration config("/etc/virtual-media.json");
    if (!config.valid)
    {
        LogMsg(Logger::Error, "Invalid configuration file,exiting");
        return -1;
    }

    if (!validateSocketPath(config))
    {
        LogMsg(Logger::Error, "Invalid socket path configuration,exiting");
        return -1;
    }
        

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
