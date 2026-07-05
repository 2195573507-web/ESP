# ESP-111 Project Audit Plan - 2026-06-16

## Goal

Perform a read-only, multi-agent audit of `/Users/zhiqin/ESP-111`, with broad coverage across firmware, backend, contracts, security, build configuration, tests, and documentation. Produce an evidence-backed audit report without modifying source code, frontend assets, or the real database.

## Constraints

- Audit only; do not modify firmware or backend implementation files.
- Report artifacts may be written under `docs/`.
- Keep ESPS3 as the only server-facing gateway.
- Treat ESPC51 and ESPC52 as lightweight local terminals.
- Do not upload or normalize raw CSI.
- Do not fake smart-home success when no real appliance exists.
- Voice turns must not block heartbeat, snapshot, logs, or other core reporting.
- Do not touch `ESP-server/public`.
- Do not touch the real `ESP-server/db/database.db`.

## Coverage Plan

- [x] Phase 1: Establish read-only scope, existing goal, and prior project constraints.
- [x] Phase 2: Spawn parallel audit agents for independent risk surfaces.
- [ ] Phase 3: Audit repository boundaries, dirty state, generated artifacts, and documentation drift.
- [ ] Phase 4: Audit end-to-end data contracts from C5 local frames through ESPS3 to ESP-server.
- [ ] Phase 5: Collect and reconcile sub-agent findings.
- [ ] Phase 6: Verify high-priority findings against source line evidence.
- [x] Phase 7: Write final audit report with severity, evidence, impact, and recommendations.
- [x] Phase 8: Close agents and complete active goal after this round.

## Agent Goals

- Agent Hegel: ESPS3 server-facing gateway boundary and data upload paths.
- Agent Mendel: ESPC51/ESPC52 lightweight terminal boundary and C51/C52 parity.
- Agent Wegener: ESP-server routes, services, DB, input validation, and contract consistency.
- Agent Carson: Voice, Realtime/WebSocket/HTTP audio, prompt cache, and gateway headers.
- Agent Pasteur: Security configuration, secrets, network exposure, logs, and dependency risk.
- Agent Lagrange: Build system, sdkconfig, partitions, generated artifacts, and reproducibility.

## Current Status

Final report is complete. The first audit round has been consolidated, late sub-agent returns were folded into the report where they added new evidence, all known agents were closed, and no follow-up audit goal was created because the user asked to stop after this round.

## Progress Update

- [x] Phase 3: Repository boundary and documentation drift audited.
- [x] Phase 4: End-to-end C5 -> ESPS3 -> ESP-server contracts audited across sensor, CSI, command, voice, dashboard, and smart-home paths.
- [x] Phase 5: First-wave sub-agent findings collected and reconciled.
- [x] Phase 6: High-priority findings verified with file/line evidence.
- [x] Phase 7: Final audit report refresh after remaining sub-agent returns.
- [x] Phase 8: Close agents and complete active goal after this round.
