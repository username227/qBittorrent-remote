# Native Add Torrent parity ledger

This feature is based on upstream qBittorrent's \`src/gui/addnewtorrentdialog.ui\`
at its current master (GPL-3.0-or-later). The native two-column layout,
management and path fields, category/tags, queue/start behavior, content
layout, metadata fields, and native Cancel/OK are preserved. The two engine-bound widgets are now adapted with their **native Qt
composition preserved** rather than replaced by a bare combo and tree:
`RemoteFileSystemPathComboEdit` retains the compact combo-plus-folder-icon
control at the original Designer coordinates. `RemoteTorrentContentWidget`
inherits the upstream `QTreeView` architecture with a hierarchical model,
folder/file icons, original three visible columns, checked but noneditable
pre-add priorities, and a filename filter at the original top-right position.
Its torrent engine handler is intentionally absent; the remote client only
shows metadata from the supplied torrent file.

| Native feature | Remote behavior |
| --- | --- |
| Add .torrent file | Local file picker → native confirmation dialog → authenticated multipart upload |
| Add magnet/HTTP(S) | Native confirmation dialog with multiline link entry → authenticated form POST |
| Save at | Remote absolute POSIX path or server default (empty); native local Browse translates through active server's configured mapping |
| Incomplete path | Same mapping/validation, optional |
| Manual/automatic management | Sent as \`autoTMM\`; automatic disables explicit path controls |
| Category/tags | Editable values sent as \`category\` and \`tags\`; live category suggestions not yet integrated |
| Start torrent | \`stopped\` parameter |
| Content layout | Original, Subfolder, NoSubfolder |
| Queue top, seed mode, sequential, first/last | Real server add parameters |
| Local .torrent metadata | Read-only name, size, infohash, date, comment and native-style QTreeView preview (icons, check states, filter); no local libtorrent engine or NAS filesystem access |
| Magnet metadata | Not pre-fetched: server downloads it after add |
| File selection/priorities | **Disabled for now**, not simulated. Web API 2.11.4 cannot pass \`filePriorities\` with uploaded .torrent; users can change priorities in bottom Content after add |
| Tag picker, default category, Never show again | Disabled, explicitly not yet implemented |
| Remember last path | Per-profile QSettings key for the initial selected server only; every in-dialog server change clears path, category, tags, and the Remember checkbox, even when switching back |
| Select destination server | Compact native selector above Torrent Management Mode only when 2+ servers are connected; submits through selected server's own authenticated session and never changes main-window tab |
| Save .torrent as | Local save of already supplied metadata |
| Duplicate .torrent or single magnet | If its v1/v2 infohash matches a torrent already in the **selected** server's live model, show native Yes/No confirmation to merge the supplied announce URLs using authenticated `POST /api/v2/torrents/addTrackers`. Do not re-add or change downloaded data. Declining does nothing. Missing tracker list produces an informative message. |
| Unknown HTTP link or unsynchronized duplicate | Do not guess: if the infohash is not yet known locally, submit normally. The Web API may return a generic rejection; do not wrongly label every failure a duplicate. |

**Safety:** The path picker operates only on locally mounted folders.
It never probes a remote server filesystem. Unmapped local paths are
rejected and never silently submitted. Paths typed directly into the
server-path fields must be absolute POSIX paths. A separate authenticated
QNetworkAccessManager/session is maintained for each connected server.
The dialogue does not bypass certificate validation or save credentials.

**Future parity work:** HTTP/Magnet metadata preview through available
server capabilities; safe selected-file priorities at add time on older
Web API versions (add stopped, set priorities, then optionally start);
live categories, tag picker, advanced stop conditions and full v2
metadata file-tree support. Maintain explicit visual differentiation
between implemented and unsupported features.

## Font-aware native layout

At ordinary text sizes the original upstream checkbox grid is unchanged.
When the two long download-option labels exceed the compact left-pane width,
the second checkbox moves to the following grid row, preserving text size
without pushing the file tree into a narrow gutter. The user can still
resize the native splitter. No application or dialog font override is used.

## Visual parity correction after first screenshot review

The initial version substituted a bare QTreeWidget and added large Browse
buttons and explanatory rows, changing the native layout significantly.
The adapted Designer file now retains the original two custom-widget slots
and their container geometries. The replacement Qt **adapters** preserve
those native shapes: small folder-icon path controls and a model-backed
QTreeView with folder/file icons, check-state presentation, native columns
and filename filtering. Remove the non-native Save at title suffix and
explanatory block, which previously pushed the information area below the
visible portion of the dialog. The torrent's metadata name is displayed in
the title as it is in native qBittorrent. Paths remain unambiguously remote
via tooltips; actual filesystem translation stays server-specific.

## Duplicate torrent and tracker merging

For private torrents, match qBittorrent 5.3.0's own duplicate behavior:
**never offer or send a tracker merge if either the existing or incoming
torrent is private**. Detect the incoming file's `info/private` flag and the
selected server's synchronized `private` field (or legacy `is_private`);
if the server does not supply a boolean privacy status, fail closed and
show an explanation instead of assuming the torrent is public.
Both branches are covered by the native file-dialog workflow test.

For a .torrent file, parse the canonical bencoded `announce` and
`announce-list` keys (bounded and deduplicated, no desktop tracker traffic).
For a single magnet, decode `urn:btih` (hex or Base32) or `urn:btmh`
identity and its `tr` parameters. Match only against the explicitly
chosen server's synchronized v1/v2 torrent hashes, never another active
tab. If found, show the native modal question **“This torrent is already
present on [server]. Do you want to merge its trackers?”**, defaulting to
No. Yes sends only the supplied tracker URLs via that server's existing
session/certificate and cookie policy. Existing data and options are not
affected. No client-side torrent engine or tracker announces are made.

When the server hasn't yet synchronized the existing torrent (or when a
plain HTTPS download link provides no pre-add infohash), this safe precheck
cannot establish duplication. Keep the existing server error handling
rather than misclassify arbitrary `Fails.` responses as duplicates.
