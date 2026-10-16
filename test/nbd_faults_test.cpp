#include "nbd_faults.hpp"

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

} // namespace

int main()
{
    testDeviceBoundary();
    testCollectFiltersAndKeepsTail();
    testChildOutputClassification();
    if (failures != 0)
    {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
