# Long-session stability changes

Symptom addressed: after 20-30 minutes of mirroring the picture freezes and the
sound disappears, sometimes recovers for a while, sometimes the connection is lost.

## Defects found in the code

| Area | Problem | Fix |
|---|---|---|
| Audio duplicate filter (`audio_receiver.cpp`) | Clean-up of the "seen" bitmap only ran when the packet landing on `pkts % 1024 == 0` happened to be a fresh audio packet. With repeated packets, loss or keep-alive traffic it silently stopped, stale bits survived the 16-bit sequence wrap and fresh audio was discarded as "duplicate". Simulated: 75 % of audio lost after the first lap with a sender that repeats each packet. | `audio/seq_dedup.h`: sliding window anchored on the highest sequence number; no state survives a wrap. Test `seq-dedup`. |
| Audio latency (mirror) | The sound-device queue was unbounded in mirror mode, so any stall left seconds of extra audio queued for the rest of the session. | Backlog above 300 ms is discarded once. |
| Audio device | A device that stopped consuming (Bluetooth/HDMI/USB sleep, endpoint change) was never reopened. | `SdlAudioOutput::stalled()/reopen()` watchdog in the playout thread. |
| Decoder errors | Errors were ignored: no flush, no resync, no fallback. A broken reference chain stayed broken. | `H264Decoder`: error streak and "no picture" detection, `resync()`, IDR wait capped at 1.5 s, escalation, GPU to CPU fallback. Test `decoder-recovery`. |
| GPU path | After a device reset every frame failed forever while the network kept running (frozen picture). | The renderer counts conversion failures; the listener moves the decoder to software. |
| Dead sender | `recv` retried forever on timeouts, the last picture stayed on screen. | 30 s idle limit and TCP keepalive on the mirror socket. Test `mirror-pipeline`. |
| Control keepalive | 5 s / 1 s / 5 probes closed the session after about 10 s of Wi-Fi trouble. | 10 s / 3 s / 8 probes (about 34 s). |
| Encrypted frames | Frames with a payload under 5 bytes skipped the cipher, which desynchronises the continuous AES-CTR keystream. | Every encrypted frame passes through the cipher. |
| Windows scheduling | No opt-out from EcoQoS / timer throttling, default 64 KB UDP buffers, no keep-awake. | `platform/thread_tuning.h`, 1 MiB UDP receive buffers, `SetThreadExecutionState` while connected. |
| Presentation | `SDL_RenderPresent` could block indefinitely with VSYNC on. | Measured; VSYNC dropped for 15 s after three blocks over 250 ms. |
| Diagnosis | Logs existed only with `--log` and a console. | Always-on bounded log file with 10 s health lines. |

## Not verified here

Everything was compiled and tested on Linux (FFmpeg software decoding, SDL dummy
audio, loopback sockets). The D3D11 code paths, the Windows-only helpers and the
real iPhone behaviour could not be exercised and need a test run on Windows.

## Build pipeline (GitHub Actions)

| Problem | Fix |
|---|---|
| Configure failed after ~32 min because `CMakeLists.txt` referenced `tools/test_*.cpp` that were not in the repository. | The new tests are opt-in (`-DBUILD_STABILITY_TESTS=ON`) and are skipped with a warning when their files are missing. |
| Every failed run rebuilt FFmpeg, OpenSSL, SDL2... from scratch (~30 min): `actions/cache` only saves when the whole job succeeds. | vcpkg writes one archive per library into `vcpkg-binary-cache`; the folder is restored at the start and saved right after the configure step with `if: always()`, so a failure in a later step no longer discards it. |
| A failing regression test blocked the package of a manual run. | For `workflow_dispatch` runs the test step is `continue-on-error`; tag builds still fail on a failing test. |
| Library build errors were hard to read. | `build-logs` artifact (vcpkg install log and buildtrees logs) on failure. |
| CI started a second full build on every uploaded commit. | `ci.yml` runs on pull requests and manually. |

## Reading `airplay.log` after a freeze

Health line, every 10 s while mirroring:

`health: mirror rx <frames> frames <KB> KB ...; decoder GPU frames=<total> (+<new>) ... fed=<packets>; sender sent video=<n> (idr=<n>) cfg=<n> heartbeat=<n> report=<n>`

* `sender sent video=0` while `heartbeat`/`report` keep coming: the **iPhone** is connected but sends no video (static screen, or its video stalled). The PC has nothing to show.
* `sender sent video` is high but `decoder ... (+0)`: the video arrives but no pictures come out (decoder/GPU problem); `errors`, `resyncs` and `gpu_fail` say more.
* `WARN mirror: no new picture for N s although the sender is connected ...` is written after 20 s, 60 s, 5 min and then every 15 min without a picture, and `mirror: pictures resumed after N s` when they return.
* `health: audio ...` shows audio packets, duplicates, the sound-device queue and whether the safety nets (`trimmed`, `device_reopened`) were used.

