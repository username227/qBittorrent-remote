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
| Behavior | Yes | Local desktop controls and live server log settings | Remaining unsupported controls |
| Downloads | Yes | Disk options and Saving Management (capability-gated remote controls) | Torrent addition defaults, backups, watched folders, exclusions and notifications |
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

Server-side fields use the authenticated session captured when Preferences opens, independent of the local path-mapping profile selector. Only keys present with representable types/values are enabled. Missing or unknown values remain explicitly unavailable; no defaults are written to unsupported servers. Edits are compared against the loaded baseline; reverting an edit removes it from the delta. Apply/OK submit only changed keys, wait for the scoped write response and fetch preferences again to verify every requested value. A rejected or unconfirmed write keeps the dialog open; any representable unapplied values remain staged. Local changes are validated first and saved only after remote confirmation (local filesystem failures are reported separately). Closing or cancelling before Apply/OK sends no write; a write in flight cannot be cancelled. Server values, including future credentials, are never stored in QSettings. The existing Behavior log controls are the first consumer; Downloads Saving Management and disk options use the same transaction.

## Downloads checkpoint 2 — disk and Saving Management

The three disk options use `preallocate_all`, `incomplete_files_ext` and `use_unwanted_folder`. Default management mode uses `auto_tmm_enabled` (true = Automatic). The three relocation policy combos use `torrent_changed_tmm_enabled`, `save_path_changed_tmm_enabled` and `category_changed_tmm_enabled` (true = Relocate, false = Switch to Manual). `use_category_paths_in_manual_mode`, `save_path`, `temp_path_enabled`, and `temp_path` are live remote values. The incomplete path is editable only if its key and the enabled toggle are supported and the toggle is checked. The folder affordance translates a selected local directory using mappings for the captured authenticated profile, never the Servers page's current mapping selection. Typed paths are absolute POSIX paths sent directly to the server; unmapped remote paths are valid and no local filesystem existence test is imposed. Actual path or incomplete-path toggle changes require confirmation only if the selected server's already-synchronized torrent records contain Auto TMM torrents or their state is unknown. A fully synchronized empty or Manual Mode-only list needs no warning. Manual category-path behavior affects suggestions, not existing locations; trailing-slash-only edits are not submitted. No extra network requests or polling are introduced. Read-back accepts equivalent canonical POSIX paths. Other Downloads controls remain disabled, and the Add Torrent dialog's defaults are addressed in checkpoint 3.

## Downloads checkpoint 3 — torrent-addition defaults

The upstream server preferences `torrent_content_layout`, `add_to_top_of_queue`, `add_stopped_enabled`, and `torrent_stop_condition` now use the same typed, authenticated preferences transaction. The stop-condition selector depends on automatic start being enabled; unknown enum values and missing keys remain unavailable. The Add Torrent dialog requests defaults once on opening and again when selecting another connected server, not periodically. It waits for the selected server's reply before enabling confirmation, ignores superseded replies, and initializes content layout, queue position, stopped state, stop condition, and the existing Automatic Torrent Management default. Explicit per-torrent overrides are sent in URL and multipart additions; a false queue override is explicit. MetadataReceived is available for links, and a file with metadata already present uses the native stopped-at-addition behavior. If the defaults are unavailable, the dialog retains the previous compatible defaults, with unsupported stop conditions omitted. Local Add Torrent dialog visibility and duplicate tracker policy remain separate follow-ups; this checkpoint does not weaken tracker privacy safeguards.

## Downloads checkpoint 4 — addition dialog and duplicate tracker behavior

Display torrent content/options, request front activation and ask before merging trackers are local desktop preferences with staged Apply/Cancel; the upstream `merge_trackers` setting is remote and typed. Confirmation overrides the default for a manually added public duplicate and uses the server preference for its suggested answer. Without confirmation, known public duplicates follow only an explicitly true server setting; private and unknown-privacy torrents remain protected, and the selected server/record is rechecked before a merge. The existing conservative duplicate-file batch skip is unchanged. Without the options dialog, file additions and dropped links go to the active server using its defaults (no per-torrent override fields); the Add URLs action still asks for links and identifies the target server. The native Never show again checkbox changes the local preference only on acceptance. Window activation is requested rather than forced, as Wayland and other desktop compositors may deny activation. Backup and watched-folder settings are the next Downloads step.
