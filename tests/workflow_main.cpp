// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>

// Each suite retains its original QTEST_MAIN setup in its own translation
// unit. Only one is invoked in a process, so globals, QApplication state,
// settings and QtTest output remain isolated between CTest registrations.
int runWindowWorkflow(int argc, char *argv[]);
int runProfileWorkflow(int argc, char *argv[]);
int runPreferencesWorkflow(int argc, char *argv[]);
int runAddTorrentWorkflow(int argc, char *argv[]);
int runSidebarFilterWorkflow(int argc, char *argv[]);
int runDetailsSortingWorkflow(int argc, char *argv[]);
int runTorrentContextMenuWorkflow(int argc, char *argv[]);
int runConnectionWorkflow(int argc, char *argv[]);
int runTlsTrustWorkflow(int argc, char *argv[]);

int main(int argc, char *argv[])
{
    struct Suite
    {
        const char *name;
        int (*run)(int, char **);
    };
    const Suite suites[] = {
        {"window", &runWindowWorkflow},
        {"profile", &runProfileWorkflow},
        {"preferences", &runPreferencesWorkflow},
        {"add_torrent", &runAddTorrentWorkflow},
        {"sidebar_filter", &runSidebarFilterWorkflow},
        {"details_sorting", &runDetailsSortingWorkflow},
        {"torrent_context_menu", &runTorrentContextMenuWorkflow},
        {"connection", &runConnectionWorkflow},
        {"tls_trust", &runTlsTrustWorkflow},
    };
    if (argc >= 2)
        for (const auto &suite : suites)
            if (std::strcmp(argv[1], suite.name) == 0)
            {
                // Strip just the suite selector, preserving argv[0] and
                // every optional QtTest method or command-line option.
                argv[1] = argv[0];
                return suite.run(argc - 1, argv + 1);
            }

    std::fprintf(stderr, "Usage: %s <suite> [QtTest arguments...]\n", argv[0]);
    std::fprintf(stderr, "Available suites:");
    for (const auto &suite : suites)
        std::fprintf(stderr, " %s", suite.name);
    std::fprintf(stderr, "\n");
    return 2;
}
