# C5/S3 Code Smell Audit Task Plan

## Goal

Scan ESPC51, ESPC52, and ESPS3 firmware code for code-smell and maintainability risks, then produce a written report. This is a read-only source audit except for the generated documentation files.

## Scope

- ESPC51 and ESPC52 C5 firmware trees.
- ESPS3 gateway firmware tree.
- Excludes generated `build/` output and third-party `managed_components/`.
- Does not modify firmware source.

## Phases

- [x] Confirm C5/S3 live code boundaries and dirty worktree state.
- [x] Collect mechanical metrics: file count, line count, large files, current diff volume, untracked runtime files.
- [x] Inspect high-risk areas: S3 runtime/network/protocol files, C51/C52 parity, firmware credentials, C5/S3 protocol boundaries.
- [x] Write final report.

## Validation

- Static audit only.
- Commands used include `find`, `wc -l`, `rg`, `git diff --stat`, `git diff --name-status`, `diff -qr`, and targeted `nl -ba` source excerpts.
- No build, flash, hardware loop, or runtime smoke test was performed.
