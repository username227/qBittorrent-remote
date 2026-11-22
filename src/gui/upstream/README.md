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
