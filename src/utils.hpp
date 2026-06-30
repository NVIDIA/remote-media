#pragma once

#include <boost/process/v1/async_pipe.hpp>
#include <boost/type_traits/has_dereference.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace utils
{
constexpr const size_t secretLimit = 1024;

// An ISO9660 image carries a Volume Descriptor Set starting at sector 16
// (offset 0x8000). Every descriptor in it begins with a type byte, the
// "CD001" standard identifier and a version byte. Reading that is the only
// way to tell a CD-ROM image from a disk image here, because neither NBD
// nor nbdkit convey a media type and no image path is available when the
// gadget is configured.
//
// Any descriptor type the standard defines for the set is accepted rather
// than requiring the primary descriptor: ECMA-119 does not fix their order,
// so a boot record may legally occupy sector 16. Being strict there would
// misreport a bootable ISO as a disk, which is the failure being fixed.
// Returns an empty optional when the image could not be read at all, so a
// caller can fall back to a weaker signal instead of assuming "not a CD-ROM".
inline std::optional<bool> isIso9660(const fs::path& device)
{
    int fd = ::open(device.c_str(), O_RDONLY);
    if (fd < 0)
    {
        return std::nullopt;
    }

    // { type, "CD001", version }
    std::array<unsigned char, 7> descriptor{};
    ssize_t bytesRead = ::pread(fd, descriptor.data(), descriptor.size(), 0x8000);
    ::close(fd);

    if (bytesRead < 0)
    {
        // The seek offset was rejected or the read itself failed (EIO, device
        // not yet ready, ...). Unlike a short read, this is not proof the
        // device is too small; report unreadable rather than "not a CD-ROM".
        return std::nullopt;
    }

    if (static_cast<size_t>(bytesRead) != descriptor.size())
    {
        // A clean short read: the device is genuinely smaller than a volume
        // descriptor set, so the answer is known even though nothing usable
        // was read.
        return false;
    }

    return descriptor[0] <= 0x03 &&
           std::memcmp(descriptor.data() + 1, "CD001", 5) == 0 &&
           descriptor[6] == 0x01;
}

// Fallback for when the image itself cannot be read, used with the URL a
// legacy mount was given. An extension only claims what the content is, so
// this is deliberately secondary to isIso9660().
inline bool hasIsoExtension(std::string_view url)
{
    // An image URL may carry a query or fragment; neither is part of the path.
    url = url.substr(0, url.find_first_of("?#"));

    constexpr std::string_view extension = ".iso";
    if (url.size() < extension.size())
    {
        return false;
    }
    url.remove_prefix(url.size() - extension.size());

    return std::equal(url.begin(), url.end(), extension.begin(),
                      [](unsigned char lhs, unsigned char rhs) {
                          return std::tolower(lhs) == rhs;
                      });
}

// Shared URL-scheme helpers.
//
// Both the mount state machine and the reboot-persistence module classify an
// image URL by scheme (file:// / https:// / smb://) and need to spot
// URL-embedded credentials. Keeping one implementation here avoids the copies
// drifting apart; callers wrap these under their own historical names.

// Case-sensitive check that `imageUrl` begins with `scheme` (e.g. "https://").
// substr(0, n) is always valid (it clamps to the string length), so a short or
// empty URL simply fails to match instead of throwing.
inline bool matchesUrlScheme(std::string_view scheme, std::string_view imageUrl)
{
    return imageUrl.substr(0, scheme.size()) == scheme;
}

// Does this URL target a local file (file:// scheme)?
inline bool isFileUrl(std::string_view imageUrl)
{
    return matchesUrlScheme("file://", imageUrl);
}

// Does this URL target an HTTPS resource (https:// scheme)?
inline bool isHttpsUrl(std::string_view imageUrl)
{
    return matchesUrlScheme("https://", imageUrl);
}

// Does this URL target an SMB/CIFS share (smb:// scheme)?
inline bool isCifsUrl(std::string_view imageUrl)
{
    return matchesUrlScheme("smb://", imageUrl);
}

// Does this URL string carry embedded userinfo credentials in its authority
// component, i.e. scheme://user[:pass]@host/...?
//
// The userinfo, if present, is the part before an '@' inside the authority
// component (which starts right after "://" and ends at the first '/', '?' or
// '#'). Restricting the search to the authority avoids false positives from an
// '@' that legitimately appears later in a path, query, or fragment.
inline bool hasEmbeddedUrlCredentials(std::string_view imageUrl)
{
    constexpr std::string_view schemeSep{"://"};
    const auto schemePos = imageUrl.find(schemeSep);
    if (schemePos == std::string_view::npos)
    {
        return false;
    }
    const auto authorityStart = schemePos + schemeSep.size();
    const auto authorityEnd = imageUrl.find_first_of("/?#", authorityStart);
    const auto authorityLen = (authorityEnd == std::string_view::npos)
                                  ? std::string_view::npos
                                  : authorityEnd - authorityStart;
    return imageUrl.substr(authorityStart, authorityLen).find('@') !=
           std::string_view::npos;
}

// Does this URL string carry a query or fragment component? For remote URLs
// these may hold secret material (e.g. pre-signed ?token=... URLs), so callers
// that store or log the URL must treat them as credential-bearing.
inline bool hasUrlQueryOrFragment(std::string_view imageUrl)
{
    return imageUrl.find_first_of("?#") != std::string_view::npos;
}

template <typename T>
static void secureCleanup(T& value)
{
    if (value.empty())
    {
        return;
    }
    auto raw = const_cast<typename T::value_type*>(value.data());
    explicit_bzero(raw, value.size() * sizeof(*raw));
}

class Credentials
{
  public:
    Credentials(std::string&& user, std::string&& password) :
        userBuf(std::move(user)), passBuf(std::move(password))
    {
    }

    ~Credentials()
    {
        secureCleanup(userBuf);
        secureCleanup(passBuf);
    }

    const std::string& user()
    {
        return userBuf;
    }

    const std::string& password()
    {
        return passBuf;
    }

  private:
    Credentials() = delete;
    Credentials(const Credentials&) = delete;
    Credentials& operator=(const Credentials&) = delete;

    std::string userBuf;
    std::string passBuf;
};

class CredentialsProvider
{
  public:
    template <typename T>
    struct Deleter
    {
        void operator()(T* buff) const
        {
            if (buff)
            {
                secureCleanup(*buff);
                delete buff;
            }
        }
    };

    using Buffer = std::vector<char>;
    using SecureBuffer = std::unique_ptr<Buffer, Deleter<Buffer>>;
    // Using explicit definition instead of std::function to avoid implicit
    // conversions eg. stack copy instead of reference for parameters
    using FormatterFunc = void(const std::string& username,
                               const std::string& password, Buffer& dest);

    CredentialsProvider(std::string&& user, std::string&& password) :
        credentials(std::move(user), std::move(password))
    {
    }

    const std::string& user()
    {
        return credentials.user();
    }

    const std::string& password()
    {
        return credentials.password();
    }

    SecureBuffer pack(const FormatterFunc formatter)
    {
        SecureBuffer packed{new Buffer{}};
        if (formatter)
        {
            formatter(credentials.user(), credentials.password(), *packed);
        }
        return packed;
    }

  private:
    Credentials credentials;
};

// Wrapper for boost::async_pipe ensuring proper pipe cleanup
template <typename Buffer>
class NamedPipe
{
  public:
    using unix_fd = sdbusplus::message::unix_fd;

    NamedPipe(boost::asio::io_context& io, const std::string name,
              Buffer&& buffer) :
        name(name),
        impl(io, name), buffer{std::move(buffer)}
    {
    }

    ~NamedPipe()
    {
        // Named pipe needs to be explicitly removed
        impl.close();
        ::unlink(name.c_str());
    }

    unix_fd fd()
    {
        return unix_fd{impl.native_sink()};
    }

    const std::string& file()
    {
        return name;
    }

    template <typename WriteHandler>
    void async_write(WriteHandler&& handler)
    {
        impl.async_write_some(data(), std::forward<WriteHandler>(handler));
    }

  private:
    // Specialization for pointer types
    template <typename B = Buffer>
    typename std::enable_if<boost::has_dereference<B>::value,
                            boost::asio::const_buffer>::type
        data()
    {
        return boost::asio::buffer(*buffer);
    }

    template <typename B = Buffer>
    typename std::enable_if<!boost::has_dereference<B>::value,
                            boost::asio::const_buffer>::type
        data()
    {
        return boost::asio::buffer(buffer);
    }

    const std::string name;
    boost::process::v1::async_pipe impl;
    Buffer buffer;
};

class VolatileFile
{
    using Buffer = CredentialsProvider::SecureBuffer;

  public:
    // size is initialised before filePath so the buffer can be moved below.
    VolatileFile(Buffer&& contents) :
        size(contents->size()), filePath(createSecure(std::move(contents)))
    {
    }

    ~VolatileFile()
    {
        // Purge file contents
        std::array<char, secretLimit> buf;
        buf.fill('*');
        std::ofstream file(filePath);
        std::size_t bytesWritten = 0, bytesToWrite = 0;

        while (bytesWritten < size)
        {
            bytesToWrite = std::min(secretLimit, (size - bytesWritten));
            file.write(buf.data(), bytesToWrite);
            bytesWritten += bytesToWrite;
        }

        // Remove leftover file
        fs::remove(filePath);
    }

    const std::string& path()
    {
        return filePath;
    }

  private:
    // mkstemp atomically creates the 0600 file, avoiding the tmpnam TOCTOU race.
    static std::string createSecure(Buffer data)
    {
        std::string tmpl =
            (fs::temp_directory_path() / "vm-cred-XXXXXX").string();
        int fd = ::mkstemp(tmpl.data());
        if (fd < 0)
        {
            throw std::system_error(errno, std::generic_category(),
                                    "mkstemp failed");
        }
        if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0)
        {
            int saved = errno;
            ::close(fd);
            ::unlink(tmpl.c_str());
            throw std::system_error(saved, std::generic_category(),
                                    "fchmod of temp credential file failed");
        }

        const char* ptr = data->data();
        std::size_t remaining = data->size();
        while (remaining > 0)
        {
            ssize_t n = ::write(fd, ptr, remaining);
            if (n < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                int saved = errno;
                ::close(fd);
                ::unlink(tmpl.c_str());
                throw std::system_error(saved, std::generic_category(),
                                        "write to temp credential file failed");
            }
            if (n == 0)
            {
                ::close(fd);
                ::unlink(tmpl.c_str());
                throw std::system_error(EIO, std::generic_category(),
                                        "short write to temp credential file");
            }
            ptr += static_cast<std::size_t>(n);
            remaining -= static_cast<std::size_t>(n);
        }
        // close() can surface a deferred write-back error (e.g. EIO) even
        // after every write() succeeded; fail construction rather than hand
        // back a path to a file that may not have persisted.
        if (::close(fd) != 0)
        {
            int saved = errno;
            ::unlink(tmpl.c_str());
            throw std::system_error(saved, std::generic_category(),
                                    "close of temp credential file failed");
        }
        return tmpl;
    }

    const std::size_t size;
    const std::string filePath;
};
} // namespace utils
