// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#pragma once

#include <sys/types.h>

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <string>
#include <vector>

// syslog(2) command numbers. glibc hides SYSLOG_ACTION_* unless
// _DEFAULT_SOURCE is set, and the unit-test image does not set it.
inline constexpr int kernelLogReadCommand = 3;
inline constexpr int kernelLogSizeCommand = 10;
inline constexpr ssize_t maxKernelLogBytes = 256 * 1024;

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

// readLog matches klogctl(command, buffer, length). The production caller
// passes klogctl; tests pass a stub so the size cap and error paths run
// without a kernel ring.
template <typename ReadLog>
inline KernelNbdFaultReport
    readRecentKernelNbdFaults(const std::string& nbdName, ReadLog&& readLog)
{
    KernelNbdFaultReport report;
    const ssize_t size =
        readLog(kernelLogSizeCommand, static_cast<char*>(nullptr), 0);
    if (size <= 0)
    {
        report.openErrno = errno;
        return report;
    }
    const ssize_t length = size > maxKernelLogBytes ? maxKernelLogBytes : size;
    std::vector<char> buf(static_cast<std::size_t>(length) + 1);
    const ssize_t byteCount =
        readLog(kernelLogReadCommand, buf.data(), static_cast<int>(length));
    if (byteCount < 0)
    {
        report.openErrno = errno;
        return report;
    }
    report.opened = true;
    collectKernelNbdFaultLines(
        report, std::string(buf.data(), static_cast<std::size_t>(byteCount)),
        nbdName);
    return report;
}
