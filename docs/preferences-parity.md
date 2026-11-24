# Preferences parity ledger

UI baseline: native qBittorrent Preferences with vertical navigation.
Navigation: **Servers first**, followed by all nine native categories in their
upstream order. The native full-width navigation item size hints center icons and labels
in IconMode, while font-aware tile heights avoid unnecessary scrolling.
Unsupported controls are disabled with explanatory tooltips, rather than
adding a persistent warning strip that occupies settings space.

The original controls remain visible and deliberately disabled until they
have a tested implementation. A disabled control's displayed default is **not
a live setting from the connected server**.

| Native section | GUI included | Functionality in this PR | Follow-up |
| --- | --- | --- | --- |
| Behavior | Yes | Unsupported controls disabled | Local confirmations, theme, transfer list actions; keep View font-size shortcuts |
| Downloads | Yes | Unsupported controls disabled | Remote default paths, torrent creation and category behavior via server capabilities |
| Connection | Yes | Unsupported controls disabled | Actual remote server preferences, never the local device's BitTorrent port |
| Speed | Yes | Unsupported controls disabled | Server speed limits and scheduler |
| BitTorrent | Yes | Unsupported controls disabled | DHT, PeX, encryption, queues and peer controls |
| Search | Yes | Unsupported controls disabled | Separate remote-search capability decision |
| RSS | Yes | Unsupported controls disabled | RSS on selected server where supported |
| WebUI | Yes | Unsupported controls disabled | Remote WebUI settings; warn before changing authentication/listening endpoint |
| Advanced | Yes | All 92 upstream Advanced setting labels in a native, scrollable, non-editable two-column view; values disabled | Enable supported settings using the selected server's remote API |
| Servers (one extra section, first) | Yes | **Implemented:** per-saved-server selector, add/edit/remove multiple paths, syntax validation, optional local mount check, transactional Apply/Cancel | Reuse mappings in Add Torrent destination picker and Open Destination Folder |

## Remote paths

Mappings are stored in Qt's existing QSettings file under server-ID-hashed
keys. Both directions use longest-directory-prefix matching with a directory
boundary check (so \`/mnt/share\` never matches \`/mnt/share-old\`).
All paths supplied to the qBittorrent server are **remote** POSIX paths; the
local path is used only to browse a mounted NAS share. No remote directory
existence can be claimed from the standard Web API.

Deleting a saved server profile **or changing its server endpoint** removes the
associated path mappings. Renaming a profile preserves its mappings.
Credentials and session cookies are not stored in Preferences.

## Acceptance criteria

- All ten navigation sections exist from the start and scroll natively.
- Unimplemented controls cannot change any local or remote configuration.
- Remote Paths exists only once and includes the server selector.
- Each profile keeps its own list of mappings, with several pairs permitted.
- Invalid or duplicate paths are rejected before writing.
- Cancel discards unapplied changes; Apply saves; OK saves and closes.
- An unmapped destination is never guessed.
- Connection, authentication and current torrent actions do not regress.

## Visual regression guardrails

- Highlighting a torrent row or its column must not change header emphasis.
  `highlightSections(false)` is applied to torrent, tracker, peer, content,
  advanced and remote-path grid headers without changing the selected row,
  sort column, sizing or native Qt font family.
- Preferences opens centered on its parent main window on first use. The
  earlier PR #9 offset position is migrated once; later manual positions are
  remembered. Entries in the vertical navigation use upstream's full-width item size
  hints (rather than a fixed grid) to center both icons and labels, with
  font-size-aware tile height.

- Native Speed navigation now uses the upstream `speedometer.svg` asset
  (not `chart-line.svg`) with 28 × 28 navigation icons, up from 25 × 25.
  Tile heights remain font-aware, and section labels/fonts do not change.

## Shared remote preferences transaction

Server-side fields use the authenticated session captured when Preferences opens, independent of the local path-mapping profile selector. Only keys present with representable types/values are enabled. Missing or unknown values remain explicitly unavailable; no defaults are written to unsupported servers. Edits are compared against the loaded baseline; reverting an edit removes it from the delta. Apply/OK submit only changed keys, wait for the scoped write response and fetch preferences again to verify every requested value. A rejected or unconfirmed write keeps the dialog open; any representable unapplied values remain staged. Local changes are validated first and saved only after remote confirmation (local filesystem failures are reported separately). Closing or cancelling before Apply/OK sends no write; a write in flight cannot be cancelled. Server values, including future credentials, are never stored in QSettings. The existing Behavior log controls are the first consumer; the Downloads controls remain disabled until subsequent PRs.
