# Debugging

## Keys in the game window

| Key | Effect |
| --- | --- |
| `F12` | Frame capture: logs the draws, saves the display and particle targets, pass inputs and outputs, and translated shaders as files (`HELSARI_CAPTURE_FULL=1`: every pass) |
| `F9` | Frame timeline for 4 frames (RSX, SPU and main-thread waits), without the capture's file dumps |
| `F11` | Toggle environment (cube-map) reflections |

## Environment variables

| Variable | Effect |
| --- | --- |
| `HELSARI_WATCHDOG=<sec>` | Periodically print every guest thread's current OS call and an RSX method histogram |
| `HELSARI_PROFILE=<sec>` | Sample all threads: CPU time per thread and hot spots (`HELSARI_PROFILE_THREADS=name,...` for more) |
| `HELSARI_TRACE_HLE=1`, `HELSARI_TRACE_RSX=1` | Log the first calls of every implemented import, or the start of the RSX command stream |
| `HELSARI_DUMP_FRAME=N`, `HELSARI_DUMP_RT=N` | Write every RSX method of frame N to `frame.txt`, or save its render targets |
| `HELSARI_DUMP_SHADERS=1` | Write all generated HLSL to `shaders/` (failed compiles are always written) |
| `HELSARI_LOG_DRAWS=N`, `HELSARI_LOG_ATTRS=d` | Per-draw state for frame N; vertex attributes and textures of draw d |
| `HELSARI_SKIP_DRAWS=a,b` | Skip draw indices (bisecting rendering bugs) |
| `HELSARI_SPU_NATIVE=0` | Run all SPU code in the interpreter |
| `HELSARI_SKIP_JOBS=job_x,...` | Run SPU jobs with empty job data (`post`: the whole post-processing set) |
| `HELSARI_GPU_TIMING=1` | Log GPU time per render target |
| `HELSARI_SYNC_READBACK=1` | Wait for the GPU on every readback for the SPUs, as before async readback |
| `HELSARI_DEBUG_PS=<hash>`, `HELSARI_DEBUG_EXPR` | Replace one pixel shader's output with an HLSL expression |
| `HELSARI_FP_WORDS=1` | Put raw fragment program words into the generated HLSL |
| `HELSARI_LOOK_LOG=1` | Log where the player, the camera and the aim points point |
| `HELSARI_GUN_TRACE=1`, `HELSARI_GUN_RAW=1` | Trace the viewmodel's pose while zoomed; draw the game's own pose untouched |

Most rendering and performance changes also have a switch to turn them off for comparison (for example
`HELSARI_CPU_VERTEX`, `HELSARI_NO_SHADER_CACHE`, `HELSARI_NO_MIPS`); they are documented where they are read in
`runtime/`, `libs/` and `port/`.

`tools/timeline.py` reads an `F9` timeline from the log.
