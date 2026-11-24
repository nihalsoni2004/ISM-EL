# Final validation report

Validation status **PASSED**. UTC interval 2026-11-24T14:23:15.9145730Z through 2026-11-24T14:27:11.9976439Z.

[Validation summary](../build/final-validation/validation-summary.json) Â· [Faculty guide](FACULTY_DEMO_GUIDE.md) Â· [Research evidence](FINAL_RESEARCH_RESULTS.md)

Debug and Release use the shared msvc-debug configuration preset and their respective build and test presets.

| Check | Passed | Failed | Skipped | Exit |
|---|---:|---:|---:|---:|
| [environment](../build/final-validation/run-20261124T142315Z-a75785b8/environment.log) | - | - | - | 0 |
| [cmake-configure](../build/final-validation/run-20261124T142315Z-a75785b8/cmake-configure.log) | - | - | - | 0 |
| [debug-build](../build/final-validation/run-20261124T142315Z-a75785b8/debug-build.log) | - | - | - | 0 |
| [debug-ctest](../build/final-validation/run-20261124T142315Z-a75785b8/debug-ctest.log) | 94 | 0 | 0 | 0 |
| [release-build](../build/final-validation/run-20261124T142315Z-a75785b8/release-build.log) | - | - | - | 0 |
| [release-ctest](../build/final-validation/run-20261124T142315Z-a75785b8/release-ctest.log) | 94 | 0 | 0 | 0 |
| [python-version](../build/final-validation/run-20261124T142315Z-a75785b8/python-version.log) | - | - | - | 0 |
| [python-pip-check](../build/final-validation/run-20261124T142315Z-a75785b8/python-pip-check.log) | - | - | - | 0 |
| [backend-pytest](../build/final-validation/run-20261124T142315Z-a75785b8/backend-pytest.log) | 31 | 0 | 0 | 0 |
| [analysis-pytest](../build/final-validation/run-20261124T142315Z-a75785b8/analysis-pytest.log) | 20 | 0 | 0 | 0 |
| [frontend-npm-ci](../build/final-validation/run-20261124T142315Z-a75785b8/frontend-npm-ci.log) | - | - | - | 0 |
| [frontend-vitest](../build/final-validation/run-20261124T142315Z-a75785b8/frontend-vitest.log) | 22 | 0 | 0 | 0 |
| [frontend-build](../build/final-validation/run-20261124T142315Z-a75785b8/frontend-build.log) | - | - | - | 0 |
| [browser-smoke](../build/final-validation/run-20261124T142315Z-a75785b8/browser-smoke.log) | - | - | - | 0 |
| [archive-research](../build/final-validation/run-20261124T142315Z-a75785b8/archive-research.log) | - | - | - | 0 |
| [documentation-hygiene](../build/final-validation/run-20261124T142315Z-a75785b8/documentation-hygiene.log) | - | - | - | 0 |
| [historical-preservation](../build/final-validation/run-20261124T142315Z-a75785b8/historical-preservation.log) | - | - | - | 0 |

Historical SHA-256 matched **1667/1667** protected files. [Preservation report](../build/final-validation/run-20261124T142315Z-a75785b8/preservation.json)

Separate optional live experiment flag was False. The archived research audit ran independently.

## Faculty rehearsal and browser evidence

The named faculty catalog passed a clean stop and restart on an alternate backend port. It retained four healthy arrays, two pools, three LUNs, and three verified original/export pairs. The adaptive job paused at 32 blocks, then completed 4,096 blocks and promoted the spare. A separate fixed-throttle run demonstrated the interactive pause hold. Thin provisioning verified allocation at 0, 64, 64 and 128 KiB. [Faculty restart evidence](../build/final-validation/faculty-walkthrough.json) · [Walkthrough and screenshots](FACULTY_DEMO_GUIDE.md#rehearsal-evidence)

The integrated Edge run covered 1440, 1024 and 390 px, adaptive scheduling, durable progress, binary download, and a controlled disconnect with recovery. Browser error count was zero. [Edge verification](../build/final-validation/run-20261124T142315Z-a75785b8/browser-demo-uMYL9t/verification.json)

The research check validated the archived Step 13 and Step 14 evidence and regenerated fourteen MEASURED plots in the run directory. A separate command successfully exercised both bounded live modes. [Research audit](../build/final-validation/run-20261124T142315Z-a75785b8/research/research-audit.json) · [Research figures](FINAL_RESEARCH_RESULTS.md#figures-and-reproduction)

## Dependency and rehearsal notes

The successful registry-backed npm install audited 147 packages and reported zero vulnerabilities. The locked install reported a `whatwg-encoding` deprecation. Backend pytest reported the existing Starlette TestClient/httpx deprecation. Validation uses a workspace-local npm cache. Earlier runs exposed a user-cache write error and a mobile route timing issue in the test harness. The retained logs document those attempts. The latest run passed after both were corrected.
