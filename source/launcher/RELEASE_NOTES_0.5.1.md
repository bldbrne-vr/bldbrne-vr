# Bldbrne-VR 0.5.1

- The performance scan lowers rendering cost when FPS is short and raises quality when there is enough room. It aims for at least 13.5% reserve, tests movement and physical attacks, and confirms its recommendation with repeated measurements.
- The scan uses the slower measured results, rejects unsupported settings, and clearly marks limited-headroom fallbacks. Normal scans usually take three to six minutes. Results estimate the tested scene; demanding areas and fights can behave differently.
- Improved compatibility for CPUs without SSE4a, including Intel CPUs.
- **Skip dangerous SFX** is enabled by default. This guarded workaround skips a problematic game effect function; disabling it can bring back effect-related crashes.

The release runtime uses ThinLTO without PGO. Normal play keeps only startup and crash diagnostics. An explicitly started performance scan collects the isolated measurement records it needs.
