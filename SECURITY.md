# Context — Security & Privacy

## Short version

* Context **does not** read browser cookies, browser profiles, browser
  authentication databases, Clerk/session tokens or saved passwords. There is no
  code in this repository that opens any browser's data folder.
* Context **does not** ask for, store, log or transmit Suno (or any provider)
  usernames, passwords, cookies, tokens or API keys.
* Context talks to a **generation bridge on your own computer** over plain
  HTTP to `127.0.0.1`. The bridge handles its own upstream sign-in; to Context it is
  just a local REST API.
* Context's **licensing server** only ever receives your Context license key, a
  hashed device ID and the plugin version. It never receives prompts, audio,
  track audio, provider credentials, cookies or session tokens.
* Track audio stays **on your computer** unless you explicitly allow and enable
  "Send Audio Context", and even then it goes only to your bridge.

## What goes where

| Data | Stays local | Generation bridge | Licensing server |
|---|---|---|---|
| Prompt you type | history.json | yes (that's the request) | **never** |
| Context-enhanced prompt, BPM, key, mix descriptors | history.json | yes | **never** |
| Track audio (rolling 4–30 s buffer, in memory) | yes | only if "Allow sending audio context" (Settings, default **off**) **and** "Send Audio Context" (Generate screen) are both on: ≤ 8 s, mono, 22.05 kHz | **never** |
| Generated audio | Generated\ folder | it's the bridge's output | **never** |
| Context license key | license.json (your AppData) | **never** | yes |
| Device ID (32-hex SHA-256 hash) | — | **never** | yes |
| Plugin version | — | no | yes |
| Provider credentials / cookies / tokens | Context never has them | Context never sends them | **never** |
| Logs | Logs\context.log (redacted) | no | no |

Local files live in `%APPDATA%\Context` (paste that into Explorer).

## Device ID

`SHA-256("context-device-v1:" + MachineGuid)`, first 32 hex characters.
`MachineGuid` is the random GUID Windows creates at install
(`HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid`). Elsewhere JUCE's
`SystemStats::getUniqueDeviceID()` is used, and as a last resort a random ID stored
in the app-data folder. No MAC addresses, serial numbers, user names or hardware
lists are collected; the raw identifier never leaves the machine.
Admins can free a device slot with `python -m app.admin reset <KEY>`.

## Licensing design

* Ed25519 signatures. The **private key exists only on the licensing server**
  (`backend/keys/signing_key.pem`, git-ignored). The plugin embeds only the public
  key (`licensing/license_public_key.txt`), so a leaked plugin binary cannot mint licenses.
* Tokens bind to the device ID and carry `refresh_after` (24 h) and `offline_until`
  (7 days). The plugin works offline until `offline_until`; network failures never
  revoke a license. A definitive server answer (revoked / expired / not activated)
  does.
* The server stores a SHA-256 hash of each license key, not the key itself
  (the full key is printed once at creation).
* License keys are only sent over HTTPS, except to `127.0.0.1`/`localhost` for testing.
* Licensing outages never mute audio, crash the DAW or touch the project: at worst
  generation is disabled.
* Admin functions are an offline CLI. The HTTP API has no admin endpoints, no
  OpenAPI/docs pages, rejects unknown fields, rate-limits per client, and uses
  parameterised SQL only.

## Bridge safety

* Default bridge address `http://127.0.0.1:3000`. Addresses on the LAN
  (`192.168.x.x`, `10.x.x.x`, `.local` ...) or the internet are **refused** unless
  the user enables "Allow non-local generation bridge" and confirms a warning for
  that specific host. Changing to another host revokes the permission.
* URLs with embedded credentials (`user:pass@host`), queries or paths are rejected.
* Bridge profiles are data only (validated paths/field names); no scripts, no hosts.
* Job IDs are validated and URL-encoded before being placed in a path.
* Every response is size-capped (2 MB JSON, 100 MB audio) and JSON nesting depth is
  checked before parsing. Malformed responses become friendly errors, never crashes.
* Audio URLs: bridge-relative paths, `http://` to loopback/the bridge host, or
  `https://` anywhere. `file://` and other schemes are refused.
* Downloads go to a temp file, are verified with a real WAV/AIFF decoder, then
  atomically renamed. Remote filenames are never used; names are generated and
  sanitised.
* JUCE's Windows networking (WinINet) is used with `INTERNET_FLAG_NO_COOKIES` and
  `INTERNET_FLAG_NO_CACHE_WRITE`, so even the OS cookie jar is not attached.

## File safety

* Cache cleanup deletes a file only if it is tracked in history, not a favorite,
  not "pinned" (dragged into a DAW, path copied, shown in Explorer, exported), is a
  direct child of the managed folder, and has a name Context generated. Exports and
  anything else are never touched.
* All JSON state (settings, history, license) is written atomically. Corrupt files
  are moved aside and replaced with defaults.

## Logging

Every log line passes through a redactor that masks license keys (keeps last 4),
`Authorization`/`Cookie` headers, bearer tokens, JWTs, `token=`/`session=`/
`password=`/`api_key=` pairs, JSON fields with secret-like names, and credentials in
URLs. Prompts and audio are not logged. "Copy Diagnostic Info" contains version,
OS, host, sample rate, bridge URL (without credentials), statuses, recent error codes
and the last 25 redacted log lines.

## Threat model (MVP)

| Threat | Mitigation | Residual risk |
|---|---|---|
| Malicious / buggy bridge response | size caps, depth check, schema validation, decoder validation, URL policy | a malicious local bridge can still return unpleasant audio |
| Bridge on the network sniffed | local-only by default, explicit opt-in with warning | opt-in LAN bridges use plain HTTP |
| License key sniffed | HTTPS required except localhost | — |
| License forgery | Ed25519, private key server-only | — |
| License cracking (binary patching) | none beyond signatures | any client-side check can be patched; acceptable for MVP |
| Clock rolled back to stretch grace | tokens issued > 2 days in the future are rejected | small windows can still be stretched |
| Local attacker reads license.json | — | key stored in plaintext in the user's own AppData (OS DPAPI encryption is future work) |
| Path traversal via filenames / job IDs | generated names, sanitiser, URL-encoding, containment checks | — |
| Deleting user files | five-condition cache rule, never outside managed folder | — |
| DoS via huge responses | 2 MB / 100 MB caps, timeouts, cancellation | — |

## Known MVP limitations

* No code signing of the Windows binary yet (SmartScreen may warn on download).
* License key stored unencrypted in `%APPDATA%\Context\license.json`.
* Rate limiting on the licensing server is in-memory, per process.
* Opt-in non-local bridges use HTTP without TLS.
* No external security audit has been performed.

## Reporting

Report security issues privately to the maintainer (not in public issues).
