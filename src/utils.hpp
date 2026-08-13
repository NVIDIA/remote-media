#pragma once

#include <boost/process/v1/async_pipe.hpp>
#include <boost/type_traits/has_dereference.hpp>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace utils
{
constexpr const size_t secretLimit = 1024;

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
