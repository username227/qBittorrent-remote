# Imported qBittorrent interface resources

The source is the qBittorrent `upstream-source` branch at commit
`133c6f24fd5bf6c2c432dc10a0cf2c2f2523425e`.

## Compiled / adapted

- `mainwindow.ui`: verbatim Qt Designer main-window layout (menus, toolbar
  and actions), compiled as part of the native Qt application.
- `hidabletabwidget.{h,cpp}`: verbatim upstream single-view tab container
  which retains the native pane frame even with its only tab label hidden.
- `transferlistfilterswidgetitem.{h,cpp}`: verbatim upstream collapsible
  status-filter section widget, compiled without local-engine dependencies.
- `proptabbar.{h,cpp}`: upstream icon-bearing properties-tab bar; adapted
  only to use our embedded Qt icons instead of upstream's theme manager.
- `proptabbar`: native bottom-anchored tab strip; clicking the active tab
  collapses only the detail pages, keeps the tabs fixed at the bottom of the
  window, and restores the user's previous splitter size when reopened.
- `trackerlist` presentation: use Qt's native hierarchical tracker rows
  with the same eleven columns as upstream TrackerListModel. Populate the
  DHT/PeX/LSD pseudo-trackers from the server Web API (never synthesize
  missing values), display their tier and announce cells as blank, and
  nest real announce endpoints under their tracker rows. Show Unix-epoch
  announce timestamps as remaining intervals, matching native qBittorrent.
  The local-engine-only tracker-tier arrows are visible but disabled until
  a supported remote mutation API is available.
- `remotepropertieswidget.ui` (in `src/gui/`): native copy of upstream
  `layouts/properties/propertieswidget.ui`, replacing the local-engine-only
  TorrentContentWidget with a Qt tree widget. Populated by Web API-backed
  general, tracker, peer, HTTP-source and content views; Speed is drawn with
  Qt from the selected remote session's actual historical rate samples.
  General's comment hyperlinks are escaped and restricted to HTTP(S).
  Content reproduces upstream's six columns, file/folder checkboxes,
  themed icons, progress bars, tri-state folder inclusion, and filename
  filter. Unchecking sends remote filePrio=0 (not filesystem deletion);
  right-click Priority and Select All/None reuse the same authenticated API.
- UI layout persistence: Qt QSettings holds main-window geometry, normal/
  maximized/minimized state, native toolbar state, sidebar/status visibility,
  active connected server, per-profile horizontal/vertical splitter state,
  per-profile torrent/content/tracker/peer header widths and ordering, sorting,
  bottom property-tab selection and collapse status, and connection/certificate/
  removal dialog geometry. No credentials, cookies or torrent metadata stored.
  Last-selected server only restores when that server is explicitly opted in
  for automatic startup connection. Wayland compositors may ignore exact
  top-level window positioning.
- Application accessibility: View → Font Size (Ctrl++ / Ctrl+=, Ctrl+−,
  Ctrl+0) controls one saved QApplication point size shared by the torrent
  list, tracker/content trees, sidebar, tab buttons, menus and dialogs.
  The original desktop/torrent-list font is the minimum; Qt theme and family
  are inherited. Sidebar list heights and transfer-list delegate caches
  are refreshed without restarting or connecting to the server.
- `progressbarpainter.{h,cpp}` and `transferlistdelegate.{h,cpp}`: adapted
  original Qt progress bars, percentages, and stable torrent row sizing.
  They read Web API-backed Qt models and the native Qt style without importing
  local BitTorrent session code, UIThemeManager or Preferences.
- `qbittorrent_icons.qrc`: path-rewritten resource manifest built from the
  complete upstream `src/icons/icons.qrc`. All referenced icons, including
  the country flags, are copied from the same upstream snapshot.

## Retained for future adaptation (not compiled)

- `layouts/*.ui` and `layouts/properties/*.ui`: other qBittorrent Qt
  Designer dialog layouts copied verbatim. Many depend on custom widgets and
  local-engine features; these serve as references for future remote API work.
- `original-icons.qrc`: original upstream icon resource manifest.

The native remote client does not link libtorrent or copy qBittorrent's
BitTorrent session implementation. Torrent information, peers, trackers,
settings, RSS, search, and other supported capabilities will be obtained
through the remote server's qBittorrent Web API, in stages.

Upstream project: https://github.com/qbittorrent/qBittorrent

All reused components retain their original copyright and GPL license terms.
See the root LICENSE. The remote application is GPL-3.0-or-later.


## Preferences UI parity (native desktop)

- `src/gui/optionsdialog.ui` is adapted from qBittorrent's GPL-3.0-or-later
  `src/gui/optionsdialog.ui` at upstream master commit 133c6f24fd5bf6c2c432dc10a0cf2c2f2523425e.
  It preserves all nine original vertical navigation sections, scrollable
  option pages, the native layout and Apply/Cancel/OK controls.
- The original Advanced page is dynamically populated upstream; this
  adaptation retains all 92 named Advanced rows in a native two-column,
  scrollable, noneditable table with disabled-value placeholders.
- `FileSystemPathLineEdit` (which depends on the local torrent engine) was
  replaced by stock `QLineEdit` while keeping its labels, properties and
  layout. Unsupported options are displayed but disabled individually;
  their hardcoded native defaults must **not** be mistaken for settings
  read from a remote qBittorrent server.
- One remote-client-only **first** section, **Servers**, contains the
  saved-server selector and its local-to-remote directory mappings together.
  The remaining nine native categories keep their upstream order. Vertical
  navigation tiles resize to the inherited application font without
  clipping; on smaller screens a native vertical scrollbar remains available.
  Legacy Preferences/LastPage settings are migrated to the new tab order.
  This supports QNAP-mounted directories without pretending that the
  Web API has a remote filesystem browser.
- Save/cancel state is local QSettings, with no password, cookie or per-torrent
  data stored. Future server settings must come from the selected session's
  authenticated `/api/v2/app/preferences` and be modified only through
  supported `/api/v2/app/setPreferences` keys.

- Consistent table header typography: disable Qt's selection-dependent
  `QHeaderView::highlightSections` on the torrent list and native Trackers,
  Peers, Content and Preferences grids. Sorting, resize and column layout
  persistence are unchanged; global font settings remain intact.
- Dialog position: initial Preferences placement (and the one-time migration
  from PR #9's earlier offset) is centered over the main window and clamped
  to the available screen. Later manual positions and sizes are preserved in
  QSettings, subject to Wayland compositor positioning rules.

- Visual fidelity: upstream `OptionsDialog` gives each IconMode navigation
  entry a full-width `QListWidgetItem::setSizeHint(QSize(INT_MAX, h))`.
  Use the same pattern, not a fixed-width grid or text alignment alone,
  to center both the icon and label at any sidebar width. The dialog
  remains freely resizable, and warning text lives on tooltips instead of
  occupying a permanent extra footer row. Preserve the user-selected
  Qt font without forcing smaller text for parity.

- Native preferences Speed icon: use upstream `speedometer.svg` with a
  modest 28 × 28 icon size. Preserve existing icon-and-label centering
  and inherited Qt fonts; do not change sidebar row spacing separately.

## Native Add Torrent

`src/gui/addnewtorrentdialog.ui` adapts upstream qBittorrent's
`src/gui/addnewtorrentdialog.ui` under its original GPL license.
Engine-bound file/path widgets use remote-specific Qt adapters **in their
original upstream Designer positions**, not bare QComboBox/QTreeWidget
substitutions. The path adapter preserves the compact combo/folder icon; the
content adapter preserves QTreeView/model, file/folder icons and metadata
filtering without binding the desktop libtorrent engine. Destinations are
mapped through `PathMappingStore`; no local download engine is instantiated. See `docs/add-torrent-parity.md` for detailed
feature and Web API limitation accounting.

- `../banlistoptionsdialog.ui`: verbatim upstream qBittorrent release-5.3.0 ban-list Designer layout, adapted by RemoteBanListDialog for staged Web API preferences. Original dialog implementation copyright (C) 2016 Alexandr Milovantsev; layout from GPL-licensed upstream. No local session dependency.
