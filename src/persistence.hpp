#pragma once

// Isolated BMC-reboot persistence module for remote-media.
//
// Design goals:
//   * Standalone: this header does NOT depend on MountPointStateMachine or
//     any FSM internals; it speaks only in std::string slot names and a
//     SavedMount POD. The state machine calls in via three narrow hook
//     points: save, clear, enumerate.
//   * Generic state directory (/var/lib/virtual-media/persist), not a
//     platform-specific (BF*/host-settings) path. The systemd unit declares
//     StateDirectory=virtual-media/persist so the kernel creates and owns
//     it 0700 root:root on first boot.
//   * Eligibility: a slot opts into
//     BMC-reboot persistence by having a non-empty AllowedLocalMountPoints
//     array in virtual-media.json. This module does NOT decide eligibility
//     itself; the FSM hook in state_machine.hpp performs that check before
//     calling writeState(). Only file:// mounts (BF4's IMAGE/CONFIG slots)
//     are persisted; remote https:// / smb:// mounts are not.
//   * Durable writes (tmp + fsync, rename, directory fsync) so a power loss
//     mid-save leaves either the old or the new file, never a half-written one.
//   * Self-healing reads: any state file that fails to parse, has the
//     wrong SchemaVersion, or has an empty / non-persistable ImageURL is
//     silently dropped by loadState() so a corrupted entry never bricks
//     startup replay.
//
// On-disk filename layout:
//   /var/lib/virtual-media/persist/<slot>.json

#include "logger.hpp"
#include "utils.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace persistence
{

namespace fs = std::filesystem;

// Default state directory. Override in tests via setStateDir(). The systemd
// unit pairs this with StateDirectory=virtual-media/persist so root:root 0700
// is provisioned automatically on every boot.
inline constexpr std::string_view defaultStateDir =
    "/var/lib/virtual-media/persist";

// Schema version for on-disk state files. Bump on incompatible changes;
// loadState() then drops mismatched files instead of mis-parsing them.
inline constexpr int stateSchemaVersion = 1;

// On-disk filenames are "<slot>.json".
// The directory is dedicated to this module (StateDirectory=virtual-media/
// persist) so there's no namespace collision with other artifacts.
inline constexpr std::string_view stateFileSuffix = ".json";

struct SavedMount
{
    std::string slot;
    std::string imageUrl;
    bool rw = false;
};

// Mutable singleton so tests can redirect the directory at a tmp path
// without touching the filesystem path constant.
inline fs::path& stateDirRef()
{
    static fs::path dir{defaultStateDir};
    return dir;
}

inline const fs::path& stateDir()
{
    return stateDirRef();
}

inline void setStateDir(const fs::path& dir)
{
    stateDirRef() = dir;
}

inline fs::path stateFile(const std::string& slot)
{
    return stateDir() / (slot + std::string{stateFileSuffix});
}

inline bool ensureStateDir()
{
    std::error_code ec;
    fs::create_directories(stateDir(), ec);
    if (ec)
    {
        LogMsg(Logger::Error, "[persistence] cannot create ", stateDir(),
               ": ", ec.message());
        return false;
    }
    // Best-effort owner-only permissions; harmless if the filesystem doesn't
    // honor the call (e.g. an in-test tmpfs already created by the harness).
    fs::permissions(stateDir(), fs::perms::owner_all,
                    fs::perm_options::replace, ec);
    return true;
}

// Only local file:// images are eligible for reboot persistence. Remote
// https:// / smb:// mounts are not persisted until Redfish
// EjectPolicy: Persistent exists. URL-embedded userinfo
// (scheme://user:pass@...) fails closed so a secret is never written.
//
// Used by both the FSM save hook and loadState(), so a remote URL is neither
// written nor replayed, including one left on disk by an older build. The
// slot's AllowedLocalMountPoints opt-in is checked separately by the FSM.
inline bool isPersistableUrl(std::string_view imageUrl)
{
    return utils::isFileUrl(imageUrl) &&
           !utils::hasEmbeddedUrlCredentials(imageUrl);
}

// fsync(2) a file or directory. ofstream flush()/close() only reaches the
// page cache; this is what makes the bytes and the directory entry durable.
inline bool fsyncPath(const fs::path& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        LogMsg(Logger::Error, "[persistence] cannot open ", path.string(),
               " for fsync: errno=", errno);
        return false;
    }
    const int rc = ::fsync(fd);
    const int err = errno;
    ::close(fd);
    if (rc != 0)
    {
        LogMsg(Logger::Error, "[persistence] fsync ", path.string(),
               " failed: errno=", err);
        return false;
    }
    return true;
}

// Durable save. Writes <slot>.json.tmp and fsyncs it, rename(2)s it over the
// target (atomic on the same filesystem), then fsyncs the directory so the
// rename itself survives power loss. Returns false on any I/O error after
// logging; callers are expected to treat that as best-effort (we never block
// the mount on a persistence failure).
inline bool writeState(const std::string& slot, const std::string& imageUrl,
                       bool rw)
{
    if (!ensureStateDir())
    {
        return false;
    }

    nlohmann::json j;
    j["SchemaVersion"] = stateSchemaVersion;
    j["Slot"] = slot;
    j["ImageURL"] = imageUrl;
    j["RW"] = rw;

    const fs::path target = stateFile(slot);
    fs::path tmp = target;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out)
        {
            LogMsg(Logger::Error, "[persistence] cannot open ", tmp.string(),
                   " for write: errno=", errno);
            return false;
        }
        out << j.dump();
        out.flush();
        out.close();
        if (!out)
        {
            LogMsg(Logger::Error, "[persistence] write failed for ",
                   tmp.string());
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }

    std::error_code ec;
    if (!fsyncPath(tmp))
    {
        fs::remove(tmp, ec);
        return false;
    }

    fs::rename(tmp, target, ec);
    if (ec)
    {
        LogMsg(Logger::Error, "[persistence] rename ", tmp.string(), " -> ",
               target.string(), " failed: ", ec.message());
        fs::remove(tmp, ec);
        return false;
    }

    if (!fsyncPath(stateDir()))
    {
        return false;
    }
    LogMsg(Logger::Info, "[persistence] saved state for slot '", slot,
           "' (image=", imageUrl, ", rw=", rw, ")");
    return true;
}

// Delete the persisted entry for a slot. Returns true if either the file was
// removed or it was already absent (the common case on a never-persisted
// slot). Logging is intentionally quiet on the absent-file path.
inline bool clearState(const std::string& slot)
{
    const fs::path target = stateFile(slot);
    std::error_code ec;
    const bool removed = fs::remove(target, ec);
    if (ec)
    {
        LogMsg(Logger::Error, "[persistence] failed to remove ",
               target.string(), ": ", ec.message());
        return false;
    }
    if (removed)
    {
        LogMsg(Logger::Info, "[persistence] cleared state for slot '", slot,
               "'");
    }
    return true;
}

// Read and validate one slot's saved state. Self-heals by deleting any file
// that fails validation; that way a future writeState starts from a clean
// slate and enumerate{...}() never resurfaces bad entries.
inline std::optional<SavedMount> loadState(const std::string& slot)
{
    const fs::path p = stateFile(slot);
    std::ifstream in(p, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }

    auto dropFile = [&p](const char* reason) {
        std::error_code ec;
        fs::remove(p, ec);
        LogMsg(Logger::Error, "[persistence] dropped ", p.string(), ": ",
               reason);
    };

    try
    {
        const auto j = nlohmann::json::parse(in, nullptr,
                                             /*allow_exceptions*/ true);
        const int ver = j.value("SchemaVersion", 0);
        if (ver != stateSchemaVersion)
        {
            dropFile("unknown SchemaVersion");
            return std::nullopt;
        }
        SavedMount sm;
        sm.slot = j.value("Slot", slot);
        sm.imageUrl = j.value("ImageURL", std::string{});
        sm.rw = j.value("RW", false);
        if (sm.slot != slot)
        {
            dropFile("slot mismatch");
            return std::nullopt;
        }
        if (sm.imageUrl.empty())
        {
            dropFile("empty ImageURL");
            return std::nullopt;
        }
        if (!isPersistableUrl(sm.imageUrl))
        {
            // Only file:// is written. Anything else (for example a remote
            // URL saved by an older build) is dropped, not replayed.
            dropFile("unsupported URL scheme for persistence");
            return std::nullopt;
        }
        return sm;
    }
    catch (const nlohmann::json::exception& e)
    {
        dropFile(e.what());
        return std::nullopt;
    }
}

// Walk the state directory once at service start and return all parseable
// SavedMount entries. Files that fail validation are removed as a side
// effect of loadState(), so subsequent boots replay only known-good slots.
inline std::vector<SavedMount> enumerateSavedSlots()
{
    std::vector<SavedMount> out;
    std::error_code ec;
    if (!fs::exists(stateDir(), ec))
    {
        return out;
    }
    for (const auto& entry : fs::directory_iterator(stateDir(), ec))
    {
        if (ec)
        {
            LogMsg(Logger::Error, "[persistence] iterate ", stateDir(),
                   " failed: ", ec.message());
            return out;
        }
        if (!entry.is_regular_file())
        {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        if (filename.size() <= stateFileSuffix.size())
        {
            continue;
        }
        if (filename.compare(filename.size() - stateFileSuffix.size(),
                             stateFileSuffix.size(), stateFileSuffix) != 0)
        {
            continue;
        }
        std::string slot =
            filename.substr(0, filename.size() - stateFileSuffix.size());
        if (auto sm = loadState(slot))
        {
            out.push_back(std::move(*sm));
        }
    }
    return out;
}

} // namespace persistence
