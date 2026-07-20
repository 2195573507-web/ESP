# Radar Isolation Audit Notes

- Canonical sources are `S3_LOCAL=0`, `C51=1`, `C52=2` in
  `ESPS3/components/Middlewares/radar_domain/radar_registry.c`.
- The active S3 state is split between local `s_spatial_state`, remote gateway
  `s_slots[2]`, and registry `s_slots[3]`; no shared tracker buffer was found.
- Existing reports predate this pass and identify missing device-bearing logs,
  unsafe Mac recent-source fallback, and incomplete room/API field coverage.
- Existing worktree changes are user-owned and must be preserved.
