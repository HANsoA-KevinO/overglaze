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
| `exposure_auto` | boolean or 0/1 | Automatic metering |
| `compare_split` | boolean or 0/1 | Diagnostic split screen: left original, right NR |

The protocol does not link `skin` and `automask`; the panel turns AutoMask on when Skin is moved, but a client must set both itself. Settings sent while NR is off are staged, and applied when NR next turns on.

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
| `nr_runtime.settings.applied_exposure_stops` | Exposure actually applied (useful with auto metering) |
| `nr_runtime.capabilities` | Evidence collected in this process (below) |
| `nr_runtime.rejected_call` | The latest skipped call: stage, reason and disposition |

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
