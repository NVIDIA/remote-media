#include "nbd_faults.hpp"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

int failures = 0;

void expect(bool cond, const char* what)
{
    if (!cond)
    {
        std::cerr << "FAIL " << what << '\n';
        ++failures;
    }
}

void testDeviceBoundary()
{
    expect(messageNamesNbdDevice("block nbd2: Connection timed out", "nbd2"),
           "nbd2 matches nbd2");
    expect(!messageNamesNbdDevice("block nbd20: Connection timed out", "nbd2"),
           "nbd20 does not match nbd2");
    expect(!messageNamesNbdDevice("block nbd2: Connection timed out", "nbd20"),
           "nbd2 does not match nbd20");
    expect(!messageNamesNbdDevice("block nbd2: Connection timed out", ""),
           "empty device name never matches");
}

void testCollectFiltersAndKeepsTail()
{
    std::string text;
    text += "block nbd2: mounted\n";
    text += "block nbd20: Connection timed out\n";
    text += "6,100,-;[  10.1] nbd2: Connection timed out 0\n";
    for (int i = 1; i <= 9; ++i)
    {
        text += "nbd2: Dead connection ";
        text += std::to_string(i);
        text += '\n';
    }
    text += "prefix-without-early-stamp [late] nbd2: shutting down sockets\n";

    KernelNbdFaultReport report;
    collectKernelNbdFaultLines(report, text, "nbd2");

    expect(report.lines.size() == kernelNbdFaultMaxLines, "keeps last 8 lines");
    expect(report.lines.front() == "nbd2: Dead connection 3",
           "drops the oldest matches");
    expect(report.lines.back().find("shutting down sockets") !=
               std::string::npos,
           "keeps the newest match");
    expect(report.lines.back().find("prefix-without-early-stamp") !=
               std::string::npos,
           "leaves a late bracket in place");

    KernelNbdFaultReport stamped;
    collectKernelNbdFaultLines(
        stamped, "6,100,-;[  10.1] nbd2: Receive control failed\n", "nbd2");
    expect(stamped.lines.size() == 1, "one stamped fault");
    expect(!stamped.lines.empty() && stamped.lines[0].starts_with('['),
           "drops the kmsg prefix before the timestamp");
}

void testChildOutputClassification()
{
    expect(isNbdLinkLossLine("NBD_CMD_READ: Broken pipe"), "broken pipe");
    expect(isNbdLinkLossLine("Connection reset by peer"), "reset");
    expect(isNbdLinkLossLine("kernel returned error"), "kernel returned");
    expect(isNbdLinkLossLine("Kernel call returned"), "Kernel call returned");
    expect(!isNbdLinkLossLine("curl: connected"), "ordinary child line");

    std::string linkLoss;
    std::string childError;
    noteChildOutputLine(linkLoss, childError, "info: started");
    noteChildOutputLine(linkLoss, childError, "error: write reply: Broken pipe");
    noteChildOutputLine(linkLoss, childError, "Connection reset by peer");
    noteChildOutputLine(linkLoss, childError, "error: later failure");

    expect(linkLoss.find("Broken pipe") != std::string::npos,
           "first link-loss line is kept");
    expect(linkLoss.find("Connection reset") == std::string::npos,
           "later link-loss line does not replace the first");
    expect(childError == "error: later failure",
           "latest error: line is kept");
}

struct ScriptedKernelLog
{
    int sizeResult = 0;
    int readResult = 0;
    int readLength = -1;
    std::string payload;
    int operator()(int command, char* buffer, int length)
    {
        if (command == kernelLogSizeCommand)
        {
            return sizeResult;
        }
        readLength = length;
        if (readResult < 0)
        {
            errno = EIO;
            return -1;
        }
        const int count = readResult < static_cast<int>(payload.size())
                              ? readResult
                              : static_cast<int>(payload.size());
        if (count > 0)
        {
            std::memcpy(buffer, payload.data(), static_cast<std::size_t>(count));
        }
        return count;
    }
};

void testReadRecentKernelNbdFaults()
{
    errno = EACCES;
    ScriptedKernelLog denied;
    denied.sizeResult = -1;
    const KernelNbdFaultReport closed =
        readRecentKernelNbdFaults("nbd2", denied);
    expect(!closed.opened, "size failure does not open the report");
    expect(closed.openErrno == EACCES, "size failure keeps errno");

    ScriptedKernelLog unread;
    unread.sizeResult = 64;
    unread.readResult = -1;
    const KernelNbdFaultReport failedRead =
        readRecentKernelNbdFaults("nbd2", unread);
    expect(!failedRead.opened, "read failure does not open the report");
    expect(failedRead.openErrno == EIO, "read failure records EIO");
    expect(unread.readLength == 64, "small ring is read in full");

    ScriptedKernelLog capped;
    capped.sizeResult = static_cast<int>(maxKernelLogBytes) + 100;
    capped.payload = "block nbd20: Connection timed out\n"
                     "block nbd2: Connection timed out\n";
    capped.readResult = static_cast<int>(capped.payload.size());
    const KernelNbdFaultReport faults =
        readRecentKernelNbdFaults("nbd2", capped);
    expect(capped.readLength == static_cast<int>(maxKernelLogBytes),
           "ring read is capped");
    expect(faults.opened, "successful read opens the report");
    expect(faults.lines.size() == 1, "stubbed ring keeps only nbd2");
    expect(!faults.lines.empty() &&
               faults.lines[0].find("nbd2: Connection timed out") !=
                   std::string::npos,
           "stubbed ring keeps the nbd2 fault");
}

} // namespace

int main()
{
    testDeviceBoundary();
    testCollectFiltersAndKeepsTail();
    testChildOutputClassification();
    testReadRecentKernelNbdFaults();
    if (failures != 0)
    {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
