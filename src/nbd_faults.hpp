#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

// Parsing helpers for NBD socket failures. Kept free of klogctl and the
// io_context so the filters can be tested without a kernel log.

struct KernelNbdFaultReport
{
    bool opened = false;
    int openErrno = 0;
    std::vector<std::string> lines;
};

inline constexpr std::size_t kernelNbdFaultMaxLines = 8;

inline bool messageNamesNbdDevice(const std::string& msg,
                                  const std::string& nbdName)
{
    if (nbdName.empty())
    {
        return false;
    }
    std::size_t pos = 0;
    while ((pos = msg.find(nbdName, pos)) != std::string::npos)
    {
        const bool boundaryBefore =
            pos == 0 ||
            !std::isdigit(static_cast<unsigned char>(msg[pos - 1]));
        const std::size_t after = pos + nbdName.size();
        const bool boundaryAfter =
            after >= msg.size() ||
            !std::isdigit(static_cast<unsigned char>(msg[after]));
        if (boundaryBefore && boundaryAfter)
        {
            return true;
        }
        pos = after;
    }
    return false;
}

inline bool isKernelNbdSocketFault(const std::string& msg)
{
    return msg.find("Connection timed out") != std::string::npos ||
           msg.find("Dead connection") != std::string::npos ||
           msg.find("shutting down sockets") != std::string::npos ||
           msg.find("Receive control failed") != std::string::npos;
}

inline bool isNbdLinkLossLine(const std::string& line)
{
    return line.find("Broken pipe") != std::string::npos ||
           line.find("Connection reset by peer") != std::string::npos ||
           line.find("Connection timed out") != std::string::npos ||
           line.find("kernel returned") != std::string::npos ||
           line.find("Kernel call returned") != std::string::npos ||
           line.find("Dead connection") != std::string::npos;
}

// First child line that shows the peer went away, and the latest "error:"
// line. Later link-loss lines stay out so the original reason is kept.
inline void noteChildOutputLine(std::string& nbdLinkLossLine,
                                 std::string& childErrorLine,
                                 const std::string& line)
{
    if (isNbdLinkLossLine(line) && nbdLinkLossLine.empty())
    {
        nbdLinkLossLine = line;
    }
    if (line.find("error:") != std::string::npos)
    {
        childErrorLine = line;
    }
}

// Keep the last kernelNbdFaultMaxLines fault lines for this device. A '['
// in the first 16 characters is the kmsg timestamp prefix and is dropped.
inline void collectKernelNbdFaultLines(KernelNbdFaultReport& report,
                                        const std::string& text,
                                        const std::string& nbdName)
{
    std::size_t begin = 0;
    while (begin < text.size())
    {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos)
        {
            end = text.size();
        }
        const std::string line = text.substr(begin, end - begin);
        begin = end + 1;
        if (!messageNamesNbdDevice(line, nbdName) ||
            !isKernelNbdSocketFault(line))
        {
            continue;
        }
        std::string shown = line;
        const auto stamp = line.find('[');
        if (stamp != std::string::npos && stamp < 16)
        {
            shown = line.substr(stamp);
        }
        report.lines.push_back(std::move(shown));
        if (report.lines.size() > kernelNbdFaultMaxLines)
        {
            report.lines.erase(report.lines.begin());
        }
    }
}
