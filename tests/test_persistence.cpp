// Unit tests for the reboot-persistence module (src/persistence.hpp).
//
// The module is intentionally standalone: it depends only on logger.hpp and
// nlohmann/json, so it can be exercised without D-Bus, systemd, udev or the
// FSM. Filesystem-touching tests redirect the state directory to a unique
// temp path via persistence::setStateDir() so they never touch the real
// /var/lib/virtual-media/persist and never collide with one another.

// utils.hpp uses sdbusplus::message::unix_fd but relies on the includer for it.
#include <sdbusplus/message/native_types.hpp>

#include "persistence.hpp"
#include "utils.hpp"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Pure URL-predicate tests (no filesystem interaction).
// ---------------------------------------------------------------------------

TEST(UrlPredicates, IsFileUrl)
{
    EXPECT_TRUE(utils::isFileUrl("file:///var/lib/img.iso"));
    EXPECT_TRUE(utils::isFileUrl("file://relative/img.iso"));
    EXPECT_FALSE(utils::isFileUrl("https://host/img.iso"));
    EXPECT_FALSE(utils::isFileUrl("smb://host/share/img.iso"));
    EXPECT_FALSE(utils::isFileUrl(""));
    // A bare prefix without "://" must not match.
    EXPECT_FALSE(utils::isFileUrl("file:/var/img.iso"));
}

TEST(UrlPredicates, IsHttpsUrl)
{
    EXPECT_TRUE(utils::isHttpsUrl("https://host/img.iso"));
    EXPECT_TRUE(utils::isHttpsUrl("https://host:8443/img.iso"));
    EXPECT_FALSE(utils::isHttpsUrl("http://host/img.iso"));
    EXPECT_FALSE(utils::isHttpsUrl("file:///var/img.iso"));
    EXPECT_FALSE(utils::isHttpsUrl(""));
}

TEST(UrlPredicates, IsCifsUrl)
{
    EXPECT_TRUE(utils::isCifsUrl("smb://host/share/img.iso"));
    EXPECT_FALSE(utils::isCifsUrl("cifs://host/share/img.iso"));
    EXPECT_FALSE(utils::isCifsUrl("https://host/img.iso"));
    EXPECT_FALSE(utils::isCifsUrl(""));
}

TEST(UrlPredicates, HasEmbeddedUrlCredentials)
{
    // Userinfo present in the authority -> credentials embedded.
    EXPECT_TRUE(
        utils::hasEmbeddedUrlCredentials("https://user:pass@host/i.iso"));
    EXPECT_TRUE(utils::hasEmbeddedUrlCredentials("https://user@host/i.iso"));
    EXPECT_TRUE(
        utils::hasEmbeddedUrlCredentials("smb://user:pass@host/share/i.iso"));
    // No path after the authority, userinfo still detected.
    EXPECT_TRUE(utils::hasEmbeddedUrlCredentials("https://user@host"));

    // '@' outside the authority (path / query / fragment) is NOT credentials.
    EXPECT_FALSE(
        utils::hasEmbeddedUrlCredentials("https://host/path@v1/i.iso"));
    EXPECT_FALSE(
        utils::hasEmbeddedUrlCredentials("https://host/i.iso?token=a@b"));
    EXPECT_FALSE(utils::hasEmbeddedUrlCredentials("https://host/i.iso#frag@x"));

    // Plain authorities and empty-authority file URLs carry no credentials.
    EXPECT_FALSE(utils::hasEmbeddedUrlCredentials("https://host/i.iso"));
    EXPECT_FALSE(utils::hasEmbeddedUrlCredentials("https://host:8443/i"));
    EXPECT_FALSE(utils::hasEmbeddedUrlCredentials("file:///local/img@1.iso"));

    // No scheme separator -> nothing to inspect.
    EXPECT_FALSE(utils::hasEmbeddedUrlCredentials("user@host"));
    EXPECT_FALSE(utils::hasEmbeddedUrlCredentials(""));
}

TEST(UrlPredicates, HasUrlQueryOrFragment)
{
    EXPECT_TRUE(utils::hasUrlQueryOrFragment("https://host/i.iso?token=x"));
    EXPECT_TRUE(utils::hasUrlQueryOrFragment("https://host/i.iso#sig"));
    EXPECT_TRUE(utils::hasUrlQueryOrFragment("smb://host/share/i.iso?a=b"));
    EXPECT_FALSE(utils::hasUrlQueryOrFragment("https://host/i.iso"));
    EXPECT_FALSE(utils::hasUrlQueryOrFragment(""));
}

TEST(UrlPredicates, IsPersistableUrl)
{
    // Only local files are eligible.
    EXPECT_TRUE(persistence::isPersistableUrl("file:///var/lib/img.iso"));

    // Remote mounts are not persisted, even without credentials.
    EXPECT_FALSE(persistence::isPersistableUrl("https://host/img.iso"));
    EXPECT_FALSE(persistence::isPersistableUrl("smb://host/share/img.iso"));

    // Embedded credentials -> not persistable.
    EXPECT_FALSE(persistence::isPersistableUrl("https://u:p@host/img.iso"));
    EXPECT_FALSE(persistence::isPersistableUrl("smb://u@host/share/img.iso"));
    EXPECT_FALSE(persistence::isPersistableUrl("file://u@host/img.iso"));

    // Remote URL with query / fragment -> not persistable.
    EXPECT_FALSE(persistence::isPersistableUrl("https://host/i.iso?token=x"));
    EXPECT_FALSE(persistence::isPersistableUrl("https://host/i.iso#sig"));
    EXPECT_FALSE(persistence::isPersistableUrl("smb://host/share/i.iso?a=b"));

    // file:// URLs are local paths, so a query/fragment does not block them.
    EXPECT_TRUE(persistence::isPersistableUrl("file:///var/lib/img.iso?x"));

    // Unsupported schemes.
    EXPECT_FALSE(persistence::isPersistableUrl("http://host/img.iso"));
    EXPECT_FALSE(persistence::isPersistableUrl("ftp://host/img.iso"));
    EXPECT_FALSE(persistence::isPersistableUrl("nfs://host/export/img.iso"));
    EXPECT_FALSE(persistence::isPersistableUrl(""));
}

// ---------------------------------------------------------------------------
// Filesystem-backed tests. Each test gets its own temp state directory.
// ---------------------------------------------------------------------------

class PersistenceFsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        static std::atomic<std::size_t> counter{0};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        tmpDir = fs::temp_directory_path() /
                 ("vm-persist-test-" + std::to_string(::getpid()) + "-" +
                  std::to_string(counter.fetch_add(1)) + "-" +
                  std::to_string(stamp));
        std::error_code ec;
        fs::remove_all(tmpDir, ec);
        persistence::setStateDir(tmpDir);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(tmpDir, ec);
        persistence::setStateDir(fs::path{persistence::defaultStateDir});
    }

    // Write raw bytes straight into a slot's state file, bypassing writeState()
    // so we can craft malformed / stale content for the self-heal tests.
    void writeRaw(const std::string& slot, const std::string& content)
    {
        std::error_code ec;
        fs::create_directories(tmpDir, ec);
        std::ofstream out(persistence::stateFile(slot),
                          std::ios::trunc | std::ios::binary);
        out << content;
        out.flush();
        out.close();
        ASSERT_TRUE(out.good()) << "Failed to write fixture state for slot: " << slot;
    }

    fs::path tmpDir;
};

TEST_F(PersistenceFsTest, StateDirAndFileNaming)
{
    EXPECT_EQ(persistence::stateDir(), tmpDir);
    EXPECT_EQ(persistence::stateFile("IMAGE"), tmpDir / "IMAGE.json");
    EXPECT_EQ(persistence::stateFile("USB1"), tmpDir / "USB1.json");
}

TEST_F(PersistenceFsTest, EnsureStateDirCreatesDirectory)
{
    EXPECT_FALSE(fs::exists(tmpDir));
    EXPECT_TRUE(persistence::ensureStateDir());
    EXPECT_TRUE(fs::is_directory(tmpDir));
    // Idempotent when the directory already exists.
    EXPECT_TRUE(persistence::ensureStateDir());
}

TEST_F(PersistenceFsTest, WriteThenLoadRoundTripReadWrite)
{
    ASSERT_TRUE(persistence::writeState("IMAGE", "file:///var/lib/img.iso",
                                        /*rw=*/true));

    auto loaded = persistence::loadState("IMAGE");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->slot, "IMAGE");
    EXPECT_EQ(loaded->imageUrl, "file:///var/lib/img.iso");
    EXPECT_TRUE(loaded->rw);
}

TEST_F(PersistenceFsTest, WriteThenLoadRoundTripReadOnly)
{
    ASSERT_TRUE(persistence::writeState("CONFIG", "file:///var/lib/cfg.iso",
                                        /*rw=*/false));

    auto loaded = persistence::loadState("CONFIG");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->imageUrl, "file:///var/lib/cfg.iso");
    EXPECT_FALSE(loaded->rw);
}

TEST_F(PersistenceFsTest, WriteStateEmitsExpectedJson)
{
    ASSERT_TRUE(persistence::writeState("USB1", "file:///var/lib/i.iso", true));

    std::ifstream in(persistence::stateFile("USB1"));
    ASSERT_TRUE(in.good());
    const auto j = nlohmann::json::parse(in);
    EXPECT_EQ(j.at("SchemaVersion").get<int>(), persistence::stateSchemaVersion);
    EXPECT_EQ(j.at("Slot").get<std::string>(), "USB1");
    EXPECT_EQ(j.at("ImageURL").get<std::string>(), "file:///var/lib/i.iso");
    EXPECT_EQ(j.at("RW").get<bool>(), true);
    // The temp sidecar must not survive a successful rename.
    EXPECT_FALSE(fs::exists(persistence::stateFile("USB1").string() + ".tmp"));
}

TEST_F(PersistenceFsTest, LoadMissingFileReturnsNullopt)
{
    EXPECT_FALSE(persistence::loadState("does-not-exist").has_value());
}

TEST_F(PersistenceFsTest, LoadDropsCorruptJson)
{
    writeRaw("IMAGE", "{ this is not valid json ");
    EXPECT_FALSE(persistence::loadState("IMAGE").has_value());
    EXPECT_FALSE(fs::exists(persistence::stateFile("IMAGE")));
}

TEST_F(PersistenceFsTest, LoadDropsWrongSchemaVersion)
{
    writeRaw("IMAGE", nlohmann::json{{"SchemaVersion", 999},
                                     {"Slot", "IMAGE"},
                                     {"ImageURL", "file:///var/x.iso"},
                                     {"RW", true}}
                          .dump());
    EXPECT_FALSE(persistence::loadState("IMAGE").has_value());
    EXPECT_FALSE(fs::exists(persistence::stateFile("IMAGE")));
}

TEST_F(PersistenceFsTest, LoadDropsEmptyImageUrl)
{
    writeRaw("IMAGE",
             nlohmann::json{{"SchemaVersion", persistence::stateSchemaVersion},
                            {"Slot", "IMAGE"},
                            {"ImageURL", ""},
                            {"RW", false}}
                 .dump());
    EXPECT_FALSE(persistence::loadState("IMAGE").has_value());
    EXPECT_FALSE(fs::exists(persistence::stateFile("IMAGE")));
}

TEST_F(PersistenceFsTest, LoadDropsNonPersistableScheme)
{
    writeRaw("IMAGE",
             nlohmann::json{{"SchemaVersion", persistence::stateSchemaVersion},
                            {"Slot", "IMAGE"},
                            {"ImageURL", "ftp://host/img.iso"},
                            {"RW", false}}
                 .dump());
    EXPECT_FALSE(persistence::loadState("IMAGE").has_value());
    EXPECT_FALSE(fs::exists(persistence::stateFile("IMAGE")));
}

TEST_F(PersistenceFsTest, LoadDropsRemoteUrlFromOlderBuild)
{
    for (const char* url : {"https://host/img.iso", "smb://host/share/img.iso"})
    {
        writeRaw("IMAGE",
                 nlohmann::json{
                     {"SchemaVersion", persistence::stateSchemaVersion},
                     {"Slot", "IMAGE"},
                     {"ImageURL", url},
                     {"RW", false}}
                     .dump());
        EXPECT_FALSE(persistence::loadState("IMAGE").has_value()) << url;
        EXPECT_FALSE(fs::exists(persistence::stateFile("IMAGE"))) << url;
    }
}

TEST_F(PersistenceFsTest, LoadDropsEmbeddedCredentialUrl)
{
    writeRaw("IMAGE",
             nlohmann::json{{"SchemaVersion", persistence::stateSchemaVersion},
                            {"Slot", "IMAGE"},
                            {"ImageURL", "https://user:pass@host/img.iso"},
                            {"RW", false}}
                 .dump());
    EXPECT_FALSE(persistence::loadState("IMAGE").has_value());
    EXPECT_FALSE(fs::exists(persistence::stateFile("IMAGE")));
}

TEST_F(PersistenceFsTest, LoadDropsQueryTokenUrl)
{
    writeRaw("IMAGE",
             nlohmann::json{{"SchemaVersion", persistence::stateSchemaVersion},
                            {"Slot", "IMAGE"},
                            {"ImageURL", "https://host/img.iso?token=secret"},
                            {"RW", false}}
                 .dump());
    EXPECT_FALSE(persistence::loadState("IMAGE").has_value());
    EXPECT_FALSE(fs::exists(persistence::stateFile("IMAGE")));
}

TEST_F(PersistenceFsTest, LoadDefaultsSlotNameWhenMissing)
{
    // Slot key omitted -> loadState falls back to the requested slot name.
    writeRaw("IMAGE",
             nlohmann::json{{"SchemaVersion", persistence::stateSchemaVersion},
                            {"ImageURL", "file:///var/x.iso"},
                            {"RW", true}}
                 .dump());
    auto loaded = persistence::loadState("IMAGE");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->slot, "IMAGE");
}

TEST_F(PersistenceFsTest, ClearRemovesExistingState)
{
    ASSERT_TRUE(persistence::writeState("IMAGE", "file:///var/x.iso", false));
    ASSERT_TRUE(fs::exists(persistence::stateFile("IMAGE")));

    EXPECT_TRUE(persistence::clearState("IMAGE"));
    EXPECT_FALSE(fs::exists(persistence::stateFile("IMAGE")));
}

TEST_F(PersistenceFsTest, ClearAbsentStateSucceeds)
{
    // Removing a slot that was never persisted is a no-op success.
    EXPECT_TRUE(persistence::clearState("never-existed"));
}

TEST_F(PersistenceFsTest, EnumerateReturnsAllGoodSlots)
{
    ASSERT_TRUE(persistence::writeState("IMAGE", "file:///var/a.iso", true));
    ASSERT_TRUE(persistence::writeState("CONFIG", "file:///var/b.iso", false));

    auto slots = persistence::enumerateSavedSlots();
    ASSERT_EQ(slots.size(), 2u);

    std::map<std::string, persistence::SavedMount> bySlot;
    for (auto& s : slots)
    {
        bySlot.emplace(s.slot, s);
    }
    ASSERT_TRUE(bySlot.count("IMAGE"));
    ASSERT_TRUE(bySlot.count("CONFIG"));
    EXPECT_EQ(bySlot["IMAGE"].imageUrl, "file:///var/a.iso");
    EXPECT_TRUE(bySlot["IMAGE"].rw);
    EXPECT_EQ(bySlot["CONFIG"].imageUrl, "file:///var/b.iso");
    EXPECT_FALSE(bySlot["CONFIG"].rw);
}

TEST_F(PersistenceFsTest, EnumerateIgnoresNonJsonFilesAndDropsBadOnes)
{
    ASSERT_TRUE(persistence::writeState("IMAGE", "file:///var/a.iso", true));
    // Non-.json file must be ignored entirely.
    writeRaw("README", "not json and no .json suffix");
    std::error_code ec;
    fs::rename(persistence::stateFile("README"), tmpDir / "README.txt", ec);
    // A .json file with bad content is dropped and excluded from results.
    writeRaw("BROKEN", "{ broken");

    auto slots = persistence::enumerateSavedSlots();
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_EQ(slots.front().slot, "IMAGE");
    // The broken entry was self-healed away.
    EXPECT_FALSE(fs::exists(persistence::stateFile("BROKEN")));
    // The unrelated file is untouched.
    EXPECT_TRUE(fs::exists(tmpDir / "README.txt"));
}

TEST_F(PersistenceFsTest, EnumerateOnMissingDirectoryReturnsEmpty)
{
    // No writes performed, so the state directory was never created.
    EXPECT_FALSE(fs::exists(tmpDir));
    EXPECT_TRUE(persistence::enumerateSavedSlots().empty());
}

TEST_F(PersistenceFsTest, WriteOverwritesExistingSlot)
{
    ASSERT_TRUE(persistence::writeState("IMAGE", "file:///var/old.iso", false));
    ASSERT_TRUE(persistence::writeState("IMAGE", "file:///var/new.iso", true));

    auto loaded = persistence::loadState("IMAGE");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->imageUrl, "file:///var/new.iso");
    EXPECT_TRUE(loaded->rw);
}
