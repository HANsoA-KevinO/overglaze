# Local control protocol 1.0

While a game runs with Overglaze, the in-game host serves a local control pipe. The in-game panel and external tools use the same controller. This page summarises the parts of the protocol that the public controller supports.

## Connection

- **Pipe:** `\\.\pipe\Overglaze.<PID>`, where `<PID>` is the game's process ID.
- **Local only:** the pipe's access list allows only the current user, and remote clients are rejected. The client checks that the server is the expected process.
- **Messages:** one JSON request, then one JSON response. At most 64 KiB per message. No image data travels over the pipe.
- **Time-outs:** an I/O time-out does not mean the request was not carried out. Ask for the status again.

## Requests

Every request is a JSON object:

```json
{
  "protocol": "1.0",
  "request_id": "set-1",
  "client_id": "my-tool",
  "session_id": "<from Hello or GetStatus>",
  "expected_revision": 12,
  "method": "SetNrSettings",
  "params": { "tone": 0.5, "structure": 1.0 }
}
```

- `request_id` and `client_id` are required, up to 128 characters each. The client ID `@embedded` is reserved for the in-game panel.
- Writes must carry the current `session_id` and `expected_revision`. Otherwise they are refused with `stale_session` or `stale_revision`. Read the status and retry with fresh values.
- **Idempotency:** sending the same `client_id` and `request_id` with the same request returns the first response and does not execute again. Reusing an ID with a different payload is refused (`id_reused`). That first response may describe an older state, so check the current status. Up to 4096 write IDs are remembered per host session; read-only requests don't count.

Responses look like this:

```json
{ "protocol": "1.0", "request_id": "set-1", "ok": true, "status": { ... } }
{ "protocol": "1.0", "request_id": "set-1", "ok": false,
  "error": { "code": "stale_revision", "message": "..." }, "status": { ... } }
```

## Who may write

There is one writer at a time.

- The in-game panel holds control by default.
- An external client must send `TakeControl` (empty `params`) before it can write. While it holds control, the panel is read-only.
- The external client's lease lasts 10 seconds and is renewed by any request from that client, for example a `GetStatus` poll. When the lease runs out, control returns to the panel and a safe OFF is requested.
- `ReleaseControl` (empty `params`) hands control back, and also requests a safe OFF.
- Overglaze never terminates the game, and never frees GPU resources that are still in use.

## Methods

| Method | Kind | Parameters |
|---|---|---|
| `Hello` | read | none. Returns the session and status. |
| `GetStatus` | read | none |
| `GetCapabilities` | read | none (same response shape as `GetStatus`) |
| `TakeControl` | write | `{}` |
| `ReleaseControl` | write | `{}` |
| `SetNrMode` | write | `{"mode": "off" \| "on" \| "compute-only"}`. NR starts `off`. `compute-only` runs NR but doesn't write the result into the game. |
| `SetNrSettings` | write | any of the fields below; an omitted field keeps its current value |

`SetNrSettings` fields:

| Field | Type and range | Meaning |
|---|---|---|
| `tone` | number, 0–2 | `DLSSNR.LocalToneStrength` |
| `structure` | number, 0–2 | `DLSSNR.LocalStructureStrength` |
| `style` | integer 0, 1 or 2 | `DLSSNR.Style` |
| `skin` | number, 0–2 | `DLSSNR.SkinStructureStrength`; only acts while AutoMask is on |
| `automask` | boolean or 0/1 | `DLSSNR.UseAutoMask` |
| `exposure_stops` | number, −12 to 10 | Overglaze's input exposure, in stops. With auto on, an offset. |
| `exposure_auto` | boolean or 0/1 | Automatic exposure: the game's own exposure when it passes a usable one, otherwise Overglaze's metering |
| `compare_split` | boolean or 0/1 | Diagnostic split screen: left original, right NR |
| `extrapolate` | boolean or 0/1 | Edit extrapolation: what is written back is this NR pass's change multiplied by `extrapolate_factor` (below). Overglaze's composite, not a model parameter |
| `extrapolate_factor` | number, 1–4 (default 2) | The multiplier n. Kept while `extrapolate` is off |

The protocol does not link `skin` and `automask`; the panel turns AutoMask on when Skin is moved, but a client must set both itself. Settings sent while NR is off are staged, and applied when NR next turns on.

With `extrapolate` on, NR still runs once per frame and keeps its own output as its history. Only what is composited back changes: in the NR API domain (the encoded image Overglaze hands the model, C, and the model's output in that domain, N), per channel, N' = clamp(C + n·(N − C), 0, 1); N' then goes through the usual ratio transfer onto the working colour. Off, or n = 1, is exactly the plain composite. This is outside what the model was designed for: sharpening overshoot and fine grain are amplified with the rest.

Image capture and diagnostic methods are not part of this release and answer `unsupported`.

## Status

`status` is a large object. The fields most useful to a client:

| Field | Meaning |
|---|---|
| `session_id`, `revision`, `pid` | Identity for writes |
| `control.source` | `in-game`, `external` or `none` |
| `nr_lifecycle.desired_mode` | What was requested |
| `nr_runtime.state` | For example `waiting-frame`, `ready`, `failed` |
| `nr_runtime.evaluates`, `nr_runtime.retired` | NR runs recorded, and NR runs completed on the GPU |
| `nr_runtime.skipped_frames`, `consecutive_skips` | Frames skipped before insertion (the game showed its own image) |
| `nr_runtime.history_gaps`, `discarded_recordings` | History resets after sequence breaks, and frames the game itself discarded |
| `nr_runtime.settings.observed` | Values the model actually read on the latest frame |
| `nr_runtime.settings.applied_exposure_stops` | Exposure actually applied (useful with automatic exposure) |
| `nr_runtime.settings.applied_extrapolate`, `applied_extrapolate_factor` | Edit extrapolation on the latest NR frame: whether it was on, and the factor the composite actually used (1 = plain composite) |
| `nr_runtime.settings.exposure_source` | Where automatic exposure took its gain on the latest NR frame: `game`, `meter`, or `manual` when automatic exposure is off |
| `nr_runtime.settings.exposure_fallback` | With `meter`: why the game's exposure was not used, by name (below); otherwise null |
| `nr_runtime.settings.game_exposure` | The game's latest values: `texture_value` (E), `pre_exposure`, `exposure_scale`, `stops` = log2(E × scale / pre-exposure), `exposed_log2_luminance` (the plausibility input) and `trusted_log2_window`; null where not read |
| `nr_runtime.settings.exposure_frames` | NR frames with automatic exposure by source (`game`, `meter`), `source_switches`, and `meter_by_reason` (counts per fallback reason) |
| `nr_runtime.capabilities` | Evidence collected in this process (below) |
| `nr_runtime.rejected_call` | The latest skipped call: stage, reason and disposition |

### Automatic exposure

With `exposure_auto` on, Overglaze prefers the exposure the game hands its upscaler: the 1×1 exposure texture E (Streamline `kBufferTypeExposure`, NGX `ExposureTexture`), DLSS `Pre.Exposure` and `Exposure.Scale`. The input exposure is then log2(E × scale / pre-exposure) plus `exposure_stops` as an offset. E is read on the GPU with one frame of latency; the game's texture is only read.

The game's value is trusted when the game-exposed image's log-average (the meter's mean log2 luminance plus that gain) lies in 2^−7.19 … 2^−0.42. The first reading decides at once; after that the source changes only after 8 consecutive readings that say so, and a trusted value is kept until it is more than one stop outside the window. Otherwise Overglaze's own meter is used, and `exposure_fallback` names why:

| Reason | Meaning |
|---|---|
| `no-exposure-texture` | The game passed no exposure texture |
| `game-uses-dlss-auto-exposure` | NGX feature created with DLSS AutoExposure |
| `exposure-tag-not-fresh`, `exposure-tag-only-valid-now`, `exposure-tag-other-thread`, `exposure-tag-invalid` | Streamline: the exposure tag was not usable for this Evaluate |
| `exposure-lease-unavailable` | No reference to the texture could be taken at Evaluate entry |
| `unsupported-format`, `unsupported-shape`, `unsupported-state` | Not a 1×1 float texture in a state Overglaze can read from |
| `exposure-aliases-an-input`, `exposure-on-another-device` | The texture is also colour, depth or motion, or belongs to another device |
| `invalid-pre-exposure-or-scale`, `invalid-exposure-value` | Pre-exposure, scale or the value read is not finite and positive |
| `implausible` | The game-exposed log-average is outside the trusted window |
| `no-meter-reading-to-verify`, `no-reading-yet` | Nothing to check the game's value against yet |
| `exposure-reader-unavailable` | Overglaze's GPU reader could not be created |

None of these is a fault: NR keeps running on the meter.

`nr_runtime.capabilities` only ever grows within a process; turning NR off doesn't undo evidence already seen.

| Key | True when |
|---|---|
| `can_load` | The bridge loaded and reported status |
| `can_toggle` | NR actually ran at least once |
| `can_set_tone`, `can_set_structure`, `can_set_style` | The model read that value during a run |
| `can_set_exposure` | NR ran (exposure is applied by Overglaze, not read by the model) |
| `can_capture` | Always false in this release |
| `can_observe_gpu` | NR work has completed on the GPU |
| `evidence_level` | `L1` loaded; `L2` NR ran; `L3` Tone, Structure and Style were all read back |

These values show that the interface took the requested values. They don't show that the image is correct.

Fields not listed here are diagnostics. They may change between releases.

## Error codes

| Code | Meaning |
|---|---|
| `bad_protocol`, `bad_request`, `bad_config` | Malformed request or parameters |
| `reserved_client` | `@embedded` used over the pipe |
| `id_reused` | Same request ID, different payload |
| `session_full` | Write-ID cache is full; restart the game to begin a new session |
| `stale_session`, `stale_revision` | Refresh the status and retry |
| `control_busy`, `takeover_required` | Another writer owns control; `TakeControl` first |
| `rebuilding` | NR resources are being rebuilt; wait for `ready` |
| `backend_failed` | NR has stopped on a fault; see `nr-fault.json` in the session folder |
| `unsupported` | Not available in this host or release |

## Command-line client


```
overglazectl <pid> <Hello|GetStatus|GetCapabilities|request.json> [client-id]
```

- With a method name, it sends a read-only request.
- With a file, it sends the JSON request in that file unchanged, so the file must carry `request_id`, `client_id`, `session_id` and `expected_revision`.
- It prints the response. Exit code `0` means `ok: true`, `3` means `ok: false`, and `2` means a usage or connection error.

Example: take control, turn NR on, hand control back.

```
overglazectl 12345 Hello                 # note session_id and revision
overglazectl 12345 take.json             # {"method":"TakeControl","params":{}, ...}
overglazectl 12345 on.json               # {"method":"SetNrMode","params":{"mode":"on"}, ...}
overglazectl 12345 release.json          # {"method":"ReleaseControl","params":{}, ...}
```

Accepted writes advance `revision`, so take the new value from each response's `status` before sending the next write.
