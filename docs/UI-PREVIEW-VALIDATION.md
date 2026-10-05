# 0.2.0-preview.1 validation — 2026-10-05

Local UI/packaging preview, now integrated with product commits `9ab53af` and
`61b527979e3f1c2ac0e21b90cfd26ef4ba0ab1c3`. Not a published release or a new game
compatibility certification. The existing installed application and game plugins
were not replaced.

## Latest source integration — 2026-10-05

Kevin authorised merging the two local product commits into `codex/product-ui`.
The source was fetched from `G:/code/overglaze`, not assumed to have reached
GitHub. The branch fast-forwarded from `12ec134` to `61b5279`; the UI work was
then reapplied. Only `controller/src/overlay.cpp` conflicted: the redesigned
layout keeps the cumulative counters and the fully wrapped latest reason under
Advanced & Diagnostics, with a visible consecutive-skip warning above the main
controls. All provider files remain byte-for-byte identical to `61b5279`.

Merge checks performed without graphics devices, game launches or NR evaluation:

| Check | Result |
| --- | --- |
| Incremental Release build | Passed; existing unused-parameter/integer-cast warnings remain |
| Controller Python suite | 60 passed, 1 expected skip; 61 total |
| Exact Streamline bindings | 2,637 checks passed |
| Workbench adapter | 86 checks passed; synthetic hooks/metadata and fixture pipe only |
| Bounded Streamline boundary | 49 checks passed |
| Streamline concurrency scope | Passed; synthetic original-call/post-return hooks, NR=0 |
| Latency markers | 61 checks passed; synthetic hooks only |
| Game-library UI | 93 checks, 92 frames, zero ImGui errors |
| Root location | 56 checks passed; no existing Lab tree writes |
| Game manager | 5,053 checks passed in a normal permission context; temporary fixtures only |
| Conflict markers / provider diff / whitespace | Clean |

The first restricted game-manager run failed while opening an ancestor directory
handle (directory busy or insufficient permissions). The unchanged executable
passed outside that restriction. It did not install into a real game.

**Known review items were not silently changed by this merge:** the separate
`hard_`/`present_` counters can still yield an old-hard/new-Present observation;
`sync()` checks only Present/loss in its outer condition, so that hard boundary
can be missed. The new adapter `hard_epoch_` comparison starts at outer return
and does not cover a boundary already consumed by an earlier `frozen_entry()`.
This is a static concurrency finding, not an observed crash or a reproduced
game failure. Also, the new skip-budget comment does not match the existing
zero-budget/no-auto-stop runtime policy. These remain upstream follow-up items;
passing the present tests does not resolve them.

No shared GPU-test mutex was bypassed. The hardware-resource lease test,
full window/overlay regression and real-game acceptance remain pending.

## Implemented

- Shared charcoal/pale-green visual system and original layered-ring mark.
- Searchable game library, status filters, contextual actions, collapsed technical
  detail, explicit target/consent confirmation, empty/loading/error states.
- In-game control layout separating requested NR state from observed state;
  existing commands, parameter ranges and backend admission remain unchanged.
- Desktop navigation, capture-browser framing, model check and local directory
  information. Viewer color processing and raw-data contracts remain unchanged.
- Eight embedded icon sizes, Windows version metadata, and model-free portable
  packaging with an allowlist, file hashes, ZIP checksum and truthful dirty state.

## Original UI preview checks (before provider integration)

The following records and archive hash belong to the earlier UI working tree on
`12ec134`; they are retained for provenance, not presented as the merged package.

| Check | Result |
| --- | --- |
| Release build | Passed |
| Controller Python suite | 60 passed, 1 skipped; 61 total |
| Game-library UI | 93 checks, 92 frames, zero ImGui errors; 1×/1.5×/2× DPI |
| Game manager | 5,053 checks on temporary fixtures; no actual game installation |
| Root location | 56 checks; 26 fixture files cleaned, no existing Lab tree writes |
| Packaged executable resources | 2 tests passed, reading PE resources as data only |
| Package contents | 32 payload files plus manifest; all payload and ZIP hashes match |
| Git whitespace check | Passed |

The Python skip is expected: the public tree does not contain the research track.
The game-manager regression cleaned only its 4,382 synthetic fixture files.
The DPI regression initially failed because its own style minimum sizes were
scaled cumulatively; the fixture now restores the full style before each scale.

The preview ZIP is approximately 3.13 MiB (7.02 MiB payload), with SHA-256:

```text
ba251ba51e5b93887d2d13766196c2a05dd5c234a00b5f2ad63b158c269c736b
```

## Still pending

An earlier iteration passed the synthetic overlay test and the four targeted
viewer/color-contract/game-library tests after a window-background fix. Those
results do not stand in for a full pass on this final revision.

The current complete window/GPU regression was not run: the shared test mutex was
occupied and Kevin confirmed another GPU test was in progress. No mutex was
bypassed, no process was ended, and no game or real NR evaluation was started for
this UI preview. The final packaged viewer has not been launched for an end-to-end
desktop check. Resume that check and the synthetic overlay regression only when
the shared GPU test is finished. Real-game UI/quality/performance acceptance is
also outstanding; this preview must not be described as fully certified.

Source remains in the isolated product worktree; the portable output is under
`data/deliverables/`. Do not overwrite an existing installation or copy its data
blindly. Migration, release publication and game deployment need separate review.
