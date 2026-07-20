# LCD/LVGL Rollback

No commit, push, pull, merge, rebase, reset, checkout, or manual managed_components edit was performed.

To roll back the formal LCD/LVGL integration:

1. Remove ESPC51/components/lcd and ESPC52/components/lcd.
2. Remove lcd from each components/Middlewares/CMakeLists.txt requirement list.
3. Remove the #include \"lcd_service.h\", non-fatal lcd_service_start() block, and its stack-monitor line from each pp_orchestrator.c.
4. Restore each pre-migration dependencies.lock through the project's normal reviewed change process; do not edit generated managed_components sources.
5. Reconfigure and build both targets. The Component Manager will resolve only the remaining declared dependencies.

The two external source directories require no rollback because their final SHA-256 manifest matches the baseline.