# Archive

Documents kept only so the reasoning behind them is not lost. **None of these describe current
state.** Do not follow instructions from them; use [`../handoff-current-status.md`](../handoff-current-status.md).

| File | What it was | Why it moved here |
|---|---|---|
| `HANDOFF-v84.md` | Short handoff written 2026-11-23 for whoever picked up after v84 | Superseded by `handoff-current-status.md`. Its status table (which version was deployed, what was blocked) is ~200 builds stale. |
| `legacy-background.md` | The Ethernet-backed companion-app era, before the kext talked to IO80211 itself | The companion app was removed; current builds ship the kext only. |
| `native-sequoia.md` | The Sequoia native line and its ABI work | The live target is Ventura 13.7. Nothing here is exercised by the current build. |
| `windows-build.md` | Cross-compiling from Windows (Zig/Clang/Cygwin) | Builds run on the Acer host via `build_v.sh`. Kept in case the host setup is ever lost. |

Rule of thumb: if a document's version number is more than ~20 builds behind the deployed one, it
belongs here, not in the index.
