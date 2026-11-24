# Authenticated functional workflow tests

The authenticated Qt GUI tests share `tests/test_connection_workflow.cpp` and its disposable HTTP server. CMake builds that executable only once, but registers its functions as independent CTest groups. No existing workflow or assertion was removed.

| CTest group | Methods at initial split | Timeout |
| --- | ---: | ---: |
| `remote_authentication_workflow` | 7 | 60s |
| `remote_server_isolation_workflow` | 2 | 60s |
| `remote_tray_and_status_workflow` | 5 | 60s |
| `remote_transfer_presentation_workflow` | 5 | 60s |
| `remote_torrent_addition_workflow` | 6 | 75s |
| `remote_content_workflow` | 7 | 75s |
| `remote_torrent_actions_workflow` | 6 | 75s |
| `remote_tracker_workflow` | 2 | 60s |

Each group runs in a fresh process with its own ephemeral server, temporary settings and offscreen Qt platform. A failure or timeout does not prevent the remaining groups from running. The registration check compares the complete QtTest function list with the configured groups and fails on omissions, duplicates or stale names.

Normal development and CI still use the existing CMake presets. For focused debugging, run a group or one method:

```sh
ctest --preset dev -R '^remote_authentication_workflow$' --output-on-failure
QT_QPA_PLATFORM=offscreen build/dev/test_connection_workflow userCanConnectAndDisconnect
```

To see all registered tests, use `ctest --preset dev -N`. To run all authenticated groups, use `ctest --preset dev -L remote --output-on-failure`. CTest can run independent processes in parallel with `--parallel 2`; the existing CI invocations remain serial initially to avoid changing scheduling and grouping at the same time.

When adding a new authenticated QtTest function, assign it to one group in CMakeLists.txt. Add another group (for example, remote Preferences) when the feature warrants its own timeout and failure report. Preserve actual user-facing GUI and server round-trip coverage; the registration integrity check is supplemental.
