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
