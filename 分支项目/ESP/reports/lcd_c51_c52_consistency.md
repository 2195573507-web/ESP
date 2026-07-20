# C51/C52 Consistency Report

- The full components/lcd file manifests are byte-identical.
- pp_orchestrator.c SHA-256 is identical across C51/C52 after the authorized six-line startup integration.
- components/Middlewares/CMakeLists.txt SHA-256 is identical across C51/C52 after adding the LCD component dependency.
- dependencies.lock SHA-256 is identical: $lockHash.
- The only final binary size difference is 16 bytes, consistent with existing device identity data rather than LCD component divergence.
- No source change was made under ESPS3 or ESP-server.