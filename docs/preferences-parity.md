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
| Behavior | Yes | Local desktop controls, completion notifications/sound and live server log settings | Remaining unsupported controls |
| Downloads | Yes | Disk options, Saving Management, addition defaults, duplicate behavior, backup settings, legacy watched folders, file-name exclusions, completion email, external program hooks and local original .torrent deletion | Unsupported recursive downloading and console controls |
| Connection | Yes | Peer protocol, incoming port, Random, UPnP/NAT-PMP, connection and upload-slot limits, I2P, proxy and IP filtering configuration | Explicit reload unavailable in Web API |
| Speed | Yes | Global and alternative rate limits, server-side scheduler, rate-limit settings | None for advertised, representable native fields |
| BitTorrent | Yes | Privacy and torrent queueing, including checking and slow-torrent limits | Seeding limits, automatic trackers |
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

## Downloads checkpoint 5 — backup metadata and watched folders

All five native .torrent backup controls bind to the upstream torrent_files_backup_enabled, torrent_files_backup_dir, torrent_files_finished_backup_dir_enabled, torrent_files_finished_backup_dir and remove_torrent_file_backup keys. Directory inputs are gated by their individual checkboxes and use the captured profile's mapped browse support. The legacy scan_dirs object supports watched-folder paths and destinations (0 = watched folder, 1 = default save path, string = custom server path). The existing native tree and Add/Options/Remove controls stage this map, not local file watchers. The richer native watched-folder options are not exposed; the deprecated upstream setter cannot reset an existing explicit path to default, so that transition is blocked. Changing the map requires confirmation because the NAS may immediately add torrents from monitored folders. A fresh read checks for intervening visible map changes before sending a replacement, followed by the normal readback; the legacy API offers no atomic compare-and-swap or visibility into hidden per-folder settings. No extra polling. Unsupported or unrepresentable keys remain disabled. Local metadata deletion is not part of server backup handling.

## Downloads checkpoint 6 — excluded file names

The native checkable group, multiline wildcard list and no-extension checkbox use `excluded_file_names_enabled`, `excluded_file_names` (newline-separated string) and `exclude_files_without_extension`. The latter is absent from the 5.1.4 and 5.3.0 APIs (introduced in Web API 2.17.0); each field is gated by the advertised type/value and the enabled parent group, not a hard-coded version cutoff. Disabling the group preserves the saved patterns and no-extension preference. No client-side filtering, file deletion, priority commands or extra polling: the selected server interprets and applies its exclusion rules. The existing Apply/Cancel and readback transaction verifies edits and never sends unsupported keys.


## Downloads checkpoint 7 — email notifications and external programs

The native email group uses typed `mail_notification_*` fields. Older servers expose Boolean `mail_notification_ssl_enabled`; newer servers expose string `mail_notification_encryption_type` (`None`, `STARTTLS`, `SMTPS`). The actual advertised capability selects the native checkbox or combo. SMTP passwords are masked and never persisted as desktop preferences. Test email sends an authenticated POST to `app/sendTestEmail` only after email edits have been applied and read back; HTTP acceptance is not delivery confirmation. The native added/finished program controls bind to `autorun_on_torrent_added_enabled`, `autorun_on_torrent_added_program`, `autorun_enabled`, and `autorun_program`. A confirmation precedes enabling hooks or editing commands, including inactive commands for safety if another client changed their state. Commands execute only on the server. Unsupported local deletion, recursive downloading and console settings remain disabled. No local process, SMTP connection or extra polling is introduced.


## Downloads checkpoint 8 — local original .torrent deletion

The two native deletion controls are desktop preferences, disabled by default, independent of server `auto_delete_mode`. They concern only original selected `.torrent` metadata files, never downloaded content or server backup files. The native Add Torrent override preserves the entire selection, including on cancellation; links have no original to delete. Explicit Cancel-button activation permits deletion only if the cancellation preference was enabled. Window close, programmatic rejection and shutdown do not delete unsubmitted files. After a successful upload response, normal synchronization on the same authenticated session must confirm every submitted torrent hash within 30 seconds, with a complete pre-upload model confirming absence. Partial batches remain intact unless all submitted hashes appear; skipped duplicates, failure and disconnect retain originals. Before removing a file, verify its captured canonical path, file type, size, modification time and SHA-256 content digest; refuse symlinks, changed or unrecognized files. No extra polling or server preference writes. A timed-out or unconfirmed operation retains metadata. This is intentionally more conservative than upstream's local-engine completion callback.

The server does not expose an atomic per-file upload receipt; matching newly visible infohashes is the available confirmation. Simultaneous additions by another client cannot be distinguished perfectly. Filesystem checks are best effort across desktop platforms, not an atomic compare-and-delete filesystem primitive.


## Legacy backup compatibility — qBittorrent 5.1.x export destinations

Older servers advertise `export_dir` and `export_dir_fin` strings rather than the five modern backup preferences. Each nonempty string enables the corresponding copy destination; disabling and applying writes an empty string, clearing the destination because the legacy API has no separate enable flag. Use the native legacy labels (copy .torrent files, and copy for finished downloads), not the modern move-backup semantics. Both share the existing mapped directory browse support. Removal of backups when removing a torrent is unsupported on these servers. Prefer modern keys when both families are advertised; never write legacy aliases alongside modern settings. The existing typed transaction verifies the actual legacy strings and rejects enabled empty/relative paths; Cancel does not clear anything. No desktop persistence of remote export paths.


## Connection checkpoint 1 — peer protocol and listening port

Native `comboProtocol` binds `bittorrent_protocol` (Both=0, TCP=1, UTP=2), `spinPort` binds `listen_port` (0=Any, up to 65535), and `checkUPnP` binds `upnp`. These are server settings, supported by both 5.1.4 and 5.3.0. Random stages a locally generated integer in the upstream inclusive 1024–65535 range; only Apply/OK writes it to the server. This does not probe port availability or claim that the port is reachable. A zero port requests OS selection on the server. The deprecated `random_port` alias is never written, and `web_ui_port` and profile endpoints are untouched. Unsupported or unrepresentable fields, including the Random button if the port field is unavailable, are disabled. The same typed delta transaction verifies read-back, with no desktop persistence. Functional two-server coverage exercises real native controls, Cancel, Random, explicit port, Any, protocol selection, forwarding, exact delta payloads, legacy/modern HTTP responses, and capability gating.


## Independent transfer text and progress colors

The desktop Behavior options for torrent-state text colors and progress-bar colors are independent. Both use the same upstream-derived light/dark palette. With text colors disabled, ordinary text follows the desktop palette and bars may still use the state palette. With progress colors disabled, the original native progress palette is used. Existing `StateColors` and `ProgressFollowsState` settings and defaults are retained; a previously checked progress-color option now takes effect even when text colors are disabled. No remote preferences are sent. The live transfer presentation test exercises the actual Preferences dialog, native delegate rendering, all four combinations, and Cancel.


## Connection checkpoint 2 — connection limits

Both qBittorrent 5.1.4 and 5.3.0 expose `max_connec`, `max_connec_per_torrent`, `max_uploads`, and `max_uploads_per_torrent`. The native checkbox/spinbox pairs represent each integer limit as one typed field: unchecking stages `-1` (unlimited); positive values enable the spinbox. Nonpositive integral values from the server are semantically unlimited as in both upstream setters, and are normalized for comparison without rewriting unchanged values. In-dialog uncheck/recheck retains the numeric value; nothing is stored locally. Connections retain the native minimum of 2; upload slots use minimum 1 rather than offering a checked value of 0 that the engine treats as unlimited. Missing, malformed or unrepresentable values disable both controls. The existing changed-key/read-back transaction handles Apply/OK/Cancel. A two-server native GUI test exercises both API response styles, every field, cancellation, exact changed keys, read-back, isolation, noncanonical unlimited and malformed/absent values.


## Connection checkpoint 3 — I2P configuration

Both upstream versions expose `i2p_enabled`, `i2p_address`, `i2p_port`, and `i2p_mixed_mode`; 5.3.0 also exposes `i2p_pex_enabled`. Native I2P controls are compile-guarded by libtorrent 2 and `TORRENT_USE_I2P`, and PeX by libtorrent >= 2.1. The API exposes preference keys regardless of those compile guards. One optional authenticated `app/buildInfo` read per Preferences opening (only when the enable key is valid) checks the server's libtorrent version through the existing serialized JSON request path. Incompatible or unidentified versions disable the group; PeX needs both its typed key and a compatible version. The API does not expose `TORRENT_USE_I2P`, so saved configuration does not establish engine support, network activity or anonymity. The original native `I2P (experimental)` heading is retained without an additional explanatory label; unavailable controls provide a reason in their tooltip. Mixed mode retains its warning about ordinary IP peers; PeX retains the native restart requirement. No automatic restart, local I2P connection or change to the Web API connection is introduced. Advanced tunnel settings remain for the Advanced page. Tests exercise actual native widgets with three authenticated simulated servers and malformed/missing capabilities, including unavailable build info without blocking unrelated controls.


## Connection checkpoint 4 — proxy configuration

Both upstream versions expose the string `proxy_type` (`None`, `SOCKS4`, `SOCKS5`, `HTTP`) and ten associated typed keys: `proxy_ip`, `proxy_port`, `proxy_auth_enabled`, `proxy_username`, `proxy_password`, `proxy_hostname_lookup`, `proxy_bittorrent`, `proxy_peer_connections`, `proxy_rss`, `proxy_misc`. The native controls follow upstream type and nested group dependencies plus individual API capability checks. None disables dependent controls without clearing stored values; SOCKS4 enables host/port and BitTorrent options, with the native unavailable-functions label; SOCKS5 and HTTP also enable authentication, hostname lookup, RSS and miscellaneous options. Authentication and peer controls depend on their respective group checks. Passwords remain masked and in the in-memory remote transaction, not desktop preferences. The upstream unencrypted-password note refers to the server's storage. These settings change the selected server, never the desktop's Web API transport or system proxy. They are not a guarantee of anonymity. Explicitly staged edits remain staged even if a parent is subsequently disabled, matching native persistence. The GUI test covers all types, dependencies, all eleven changed keys, Cancel, readback, isolation, unsupported values, form encoding and absence of passwords in desktop settings.

## Desktop completion notifications

The native Behavior page adds a Notifications group below the notification-area controls. Desktop notifications default on; the independent bundled completion sound defaults off. Both settings are global local preferences and take effect on Apply/OK, not on Cancel. Each connected session, including inactive tabs, tracks incomplete torrents and newer positive completion timestamps using the existing merged sync data. A full snapshot establishes a silent baseline (including reconnect or recovery); new already-complete torrents, removal/re-add, repeated deltas and check progress do not replay old notifications. This intentionally reports observed transitions rather than historical completions while disconnected. Linux uses freedesktop notifications without needing a tray icon; other desktops use native Qt tray messages, with a temporary icon if the permanent tray is disabled. Notification delivery and shell sounds remain subject to OS settings; the bundled original WAV is played locally through Qt Multimedia, with system beep fallback on decode/load error. No NAS process, preference write, additional polling or external audio file is needed. CTest disconnects the notifier's own OS/audio delivery slots before connecting either simulated server. The real sync, preference, and delivery-request workflow still runs and is verified by signal spies, but ordinary test runs never display notifications or play sounds even inside a live desktop session. Physical playback and OS presentation are manually tested.

## Connection checkpoint 5 — IP filtering

Both supported API generations expose `ip_filter_enabled`, `ip_filter_path`, `ip_filter_trackers` and the newline-separated replacement string `banned_IPs`. Native filter enable/path dependencies are preserved; tracker filtering and the manually banned list remain independent. Browse selects a mounted file (.dat/.p2p/.p2b) translated through the captured server's mappings, not a directory or upload. Manual paths are absolute server paths; enabling requires a nonempty file path. Preference readback confirms stored configuration only, not successful filter parsing or enforcement. The native reload button remains disabled with an explanatory tooltip because the API cannot explicitly reload an unchanged path; clearing and restoring it could leave the server unprotected.

The exact upstream 5.3.0 ban-list Designer layout is reused with remote staging. 5.1.4 and 5.2.0 accept only individual addresses; 5.3.0 adds ranges/CIDR. Gate range editing on the authenticated server version. Canonicalize IPv4/IPv6 and ranges, sort, prevent duplicates, and disable editing malformed/unrepresentable server lists instead of silently dropping entries. Child OK stages the list; outer Apply/OK writes it, unlike the original immediate local-engine mutation. Before replacement, re-read the list and refuse visible concurrent edits; this cannot eliminate the race after read because the API has no compare-and-set. No filter file upload, local enforcement, or server reload is simulated.

## Speed preferences — complete native page

The four native KiB/s spin boxes bind to the server's byte-per-second `up_limit`, `dl_limit`, `alt_up_limit`, and `alt_dl_limit`. Zero is unlimited. Exact multiples of 1024 within the native 0–2,000,000 KiB/s range are representable; malformed, missing or fractional-KiB values are unavailable rather than rounded to a misleading limit. Both upstream API generations require complete hour/minute pairs when updating `schedule_from_hour` / `schedule_from_min` or `schedule_to_hour` / `schedule_to_min`; only changed times send a pair. A missing/invalid component disables that entire time. The schedule enable checkbox and native day choices (Every day, Weekdays, Weekends, Monday–Sunday, 0–9) use `scheduler_enabled` and `scheduler_days`. Unchecking retains dormant values; equal times are rejected when editing the schedule, and enabling requires complete valid schedule fields. Overnight schedules are permitted. Times refer to the server's local clock, not the desktop. The three rate-limit options directly bind `limit_utp_rate`, `limit_tcp_overhead`, and `limit_lan_peers`. The shared Apply/OK changed-key transaction verifies readback, preserves Cancel and server isolation, and never invokes a local scheduler or adds polling. Functional CTest exercises all controls, units, unlimited, max rate, paired-time payloads, equal-time rejection, overnight times, day choices, disabled schedule preservation, malformed/missing fields and server isolation with both API generations. Actual time-based enforcement remains a server integration concern.

## BitTorrent checkpoint 1 — Privacy

Both upstream 5.1.4 and 5.3.0 expose the same boolean preferences `dht`, `pex`, `lsd`, and `anonymous_mode` and integer `encryption` (0 allow, 1 require, 2 disable). Bind the five native controls directly to their typed values from the captured authenticated server; absent, malformed or unknown values remain unavailable and are never replaced with Designer defaults. Preserve the original encryption choices and anonymous-mode information link. These independent server-wide settings do not toggle the desktop VPN or proxy, override private-torrent restrictions, or guarantee anonymity. Changes use the shared changed-key Apply/OK/readback transaction; Cancel never writes, and no extra polling or local persistence is introduced. Functional CTest covers both API generations, all five controls, encryption indices, unchanged-value reversion, Cancel, exact changed-key payloads, readback, selected-server isolation and unavailable fields. Actual peer discovery and encryption enforcement remain server integration concerns.

## BitTorrent checkpoint 2 — Torrent Queueing

Upstream 5.1.4 and 5.3.0 expose nine independent typed preferences: `max_active_checking_torrents`, `queueing_enabled`, `max_active_downloads`, `max_active_uploads`, `max_active_torrents`, `dont_count_slow_torrents`, `slow_torrent_dl_rate_threshold`, `slow_torrent_ul_rate_threshold`, and `slow_torrent_inactive_timer`. Use the original native spin-box ranges (-1 through INT_MAX for active limits, 0–2,000,000 KiB/s for rate thresholds, 1–999,999 seconds for inactivity). Unlike connection limits, zero is distinct from -1 (unlimited); unlike Speed preferences, the API's slow-rate thresholds are already in KiB/s. Checking is independent of queueing; active limits and the slow-torrent group depend on supported, enabled queueing, and threshold/timer controls additionally depend on supported, enabled slow-torrent exclusion. Unchecking preserves stored and staged dormant values. The native slow-torrent exclusion label is retained; its tooltip explains that the individual download/upload limits exclude slow torrents while the overall active-torrent limit remains a hard cap. Missing, invalid or nonrepresentable fields remain disabled. Apply/OK sends only changed keys, verifies readback and preserves server isolation; Cancel writes nothing. No local queueing engine, timer or polling is introduced. Functional CTest exercises both versions, native dependencies and editors, zero/unlimited/maximum values, units, reversion, Cancel, exact changed-key payloads, dormant values, missing/malformed fields and server isolation. Actual queue enforcement is not simulated or verified by preference readback.

## WebUI checkpoint 1 — server policy and live connection settings

Verified the typed preference fields against upstream 5.1.4 and 5.3.0. The outer WebUI group is no longer checkable: the remote client cannot disable its own API. Router forwarding, authentication bypass and subnet whitelist, ban policy, session timeout/count, alternative WebUI, clickjacking and CSRF protection use the existing selected-server staged delta/readback flow with explicit confirmation. The whitelist editor validates IP/CIDR entries and checks for concurrent changes before replacing the list. The 5.3-only session-count and API-key controls are hidden when absent. API keys and DNS credentials are masked and never persisted locally. Bind address, port, HTTPS/certificate paths, username, secure cookies, host validation/domains, custom headers, reverse-proxy settings and Dynamic DNS are live read-only for this checkpoint; password and API-key mutation are deferred until the connection-transition workflow is implemented. No WebUI enable or disable key is written. Authenticated two-server QtTest covers capability display, staged cancel, exact delta, readback, security confirmation and secret exclusion from desktop settings. Actual endpoint or credential transitions are not implemented by this checkpoint.

## WebUI checkpoint 2 — direct connection transition

For profiles whose scheme, port and root path match the server's live listener settings and which do not advertise reverse-proxy support, the native bind address, port, HTTPS/certificate paths and username can now be staged. The write-only password can be changed regardless of either authentication bypass switch; a blank field means no change. Use the staged new password directly without asking for the old password or requiring re-entry. Connection edits must be applied separately. Confirm the exact proposed same-host endpoint; request existing connection credentials only when no new password was entered. HTTP is explicitly identified as unencrypted. When bypass may apply, require an acknowledged password write and successful fresh connection/readback, and explicitly warn that the password itself was not independently verified; an ambiguous write cannot be accepted on that basis. A fresh preference read checks for concurrent connection/security changes, a single write is attempted, and a fresh isolated session verifies the new endpoint and expected preference values even when the old response was lost. New certificate trust requires explicit approval; the saved profile is not updated until verification. Verified same-host direct transitions preserve path mappings; pins remain exact-URL scoped. Failure does not imply rollback: keep the old profile and require independent recovery. Reverse-proxy/external port mappings, API key mutation, host/cookie/custom header and Dynamic DNS edits remain deferred. A fresh main session reconnects after temporary probe logout; credential storage uses the existing secure vault policy. No passwords are stored in QSettings.

## WebUI checkpoint 3 — security and reverse proxy

Both qBittorrent 5.1.4 and 5.3.0 expose the seven typed cookie, Host header, domain, custom HTTP header and trusted reverse-proxy fields. Native checkable groups retain dormant values, with semicolon-separated domains/proxies and one header per line. Validate active lists before writing, keep the reverse-proxy documentation link available even when unchecked, and require independent recovery access and a fresh isolated connection/readback at the existing profile endpoint. Security writes are staged separately from listener and credential transitions; they do not rewrite the profile or remembered credential. A failed verification cannot establish rollback. API key and Dynamic DNS remain deferred. Functional GUI coverage uses two authenticated simulated daemons, exact changed-key payload, Cancel, ignored write detection and unchanged server profiles/credentials. Live reverse-proxy deployment and real server policy enforcement remain manual verification tasks.
