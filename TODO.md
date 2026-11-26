# To do

## Deferred: native local Search subsystem

Implement a complete desktop-side Search interface after the initial release, rather than exposing standalone remote Search preferences.

### Rationale

Remote qBittorrent installations, especially NAS and minimal container deployments, may not have Python 3 installed or enabled. Interactive searches should not require Python or search plugins on each remote server. RSS automation remains server-side because it must run independently of the desktop client.

### Approach

Port the existing upstream qBittorrent Search subsystem where practical, preserving its native Qt interface, plugin compatibility, and upstream license notices. Use the upstream [release-5.3.0 source tree](https://github.com/qbittorrent/qBittorrent/tree/release-5.3.0) as a reference; evaluate the current appropriate upstream version at implementation time.

- [ ] Audit dependencies and reuse the native widgets, result models, search tabs, and plugin management dialogs from `src/gui/search/`.
- [ ] Adapt the search handlers and plugin management from `src/base/search/` and the Python/Nova engine from `src/searchengine/nova3/` (including its resources). Keep changes to upstream code minimal and document adaptations.
- [ ] Execute Python and search plugins locally on the desktop; no remote Python dependency. Investigate Python 3 as an optional Arch dependency and interpreter discovery or bundling on Windows. Handle missing Python gracefully.
- [ ] Adapt upstream settings, plugin directories, job/result persistence, search history, and tab restoration to local application settings and storage. Treat plugins as executable code and retain appropriate installation/trust safeguards.
- [ ] Route torrent and magnet additions through the existing native remote addition workflow, explicitly targeting the selected server. Handle locally downloaded torrent files where required by plugins. Never download torrent payloads locally as a consequence of Search.
- [ ] Make the same local plugin collection available across server connections. Verify that switching servers cannot accidentally add a result to the wrong destination.
- [ ] Provide the complete Search interface and relevant local preferences together. Review upstream behavior and preserve visual and functional parity where practical.
- [ ] Add functional tests covering plugin execution, results, persistence, absent Python, and remote destination routing without requiring live trackers; verify Arch and Windows CI and manually test both platforms.

### Deferred PR

Leave [PR #75](https://github.com/username227/qBittorrent-remote/pull/75) open and unmerged for now. Its server-backed Search preferences are not useful without a Search interface and would not govern a local Search implementation. Reassess that work when the complete subsystem is implemented; do not merge those controls on their own.

## Deferred: interface localization

Keep the application English-only during active development. Full localization is not a requirement for the initial release. Revisit once the interface is stable and community contributors are available; avoid committing the maintainer to maintaining every upstream language catalog.

- [ ] Continue marking user-visible strings for translation with Qt's `tr()` and Designer translation support.
- [ ] When revisiting localization, extract the current application strings with Qt Linguist, reuse verified upstream translations (including renamed contexts where meaning is unchanged), and identify modified or remote-specific strings requiring new translations.
- [ ] Establish an incremental, community-supported translation workflow; accept languages with maintainable coverage and review rather than promising support for every upstream locale.
- [ ] Verify translation loading, fallback, Apply/Cancel, restart behavior and packaging on supported platforms.

[PR #98](https://github.com/username227/qBittorrent-remote/pull/98) is a deferred prototype, not intended for merge now. Do not introduce its partial German catalog, language selector or new build dependency into the initial release. Proceed with Phase 3 torrent properties parity.
