# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

BleatBox is Zephyr RTOS firmware for Adafruit Feather nRF52840 Express boards that trigger synchronized sound playback over BLE when any device detects motion. Hardware: Adafruit Feather nRF52840 Express, Music Maker FeatherWing (VS1053B codec + SD card), LIS2DW12 accelerometer.

## Build Commands

```bash
make            # Run tests, then build firmware (incremental)
make test       # Host-native unit tests only (GoogleTest via CMake/CTest)
make clean      # Remove build/ and build-tests/
make flash      # west flash (requires user to double-press reset button to put board into UF2 mode first)
```

`make` runs tests first, then `west build` inside the project's `.venv`. The board target defaults to `adafruit_feather_nrf52840/nrf52840/uf2` and can be overridden with `BOARD=`.

## Architecture

Event-driven main loop with decoupled producer modules:

- **Producers** (accel, BLE) push events into a shared `k_msgq` via `K_NO_WAIT` — they have no knowledge of the audio subsystem or each other.
- **Main loop** is the sole consumer: blocks on `k_msgq_get`, dispatches to handlers.
- **Audio** runs on a dedicated background thread; `audio_play_sound()` signals and returns immediately. Handlers gate on `audio_is_playing()` to drop events during playback.

Adding a new trigger source means: create a module that calls `k_msgq_put` with an event, add a case to the main loop's switch, and wire up init in `main()`.

### Module lifecycle (two-phase init)

Modules separate **setup** from **operation**, and `main()` runs the whole fleet through both phases in order:

- **`<module>_init()`** — initialize internal state and, optionally, bring up the hardware peripheral. No side effects: it must not start producing events, start scanning, arm interrupts, or otherwise *use* the peripheral. It is config-independent (takes no values from `bleatbox.cfg`), so phase 1 runs before the SD card is even mounted. A module that can fail records its readiness in a `static bool g_ready` (or similar) set here.
- **The operate phase** — a second function (`<module>_start(...)`, `sounds_scan()`, `sdcard_mount()`, `audio_play_sound()`, …) does the work that uses the peripheral and takes any config-derived parameters. It gates on the readiness flag so it is a safe no-op when init failed.

`main()` therefore reads top-to-bottom as: **phase 1** — call every `_init()`; **phase 2** — load config, then call the operate/`_start()` functions. Producers (the accelerometer trigger, the BLE scan) come alive only at the very end, so no event can arrive before every module is ready.

Every init step except the event queue itself is **non-fatal**: a failed subsystem logs an error and `main()` continues, disabling only its own feature (no codec → playback fails gracefully; no SD card → default config; no accelerometer → no vibration triggers). This keeps the shell and the remaining subsystems available for debugging in the field. Pure-state modules (`assignments`, `broadcast_log`) and stateless helpers (`device_config`, `wrap_to_range`, `sounds_match`) have only the relevant phase and no separate `_init()`.

### Configuration

`/SD:/bleatbox.cfg` is a line-oriented text config parsed by `device_config.c`. Each device runs identical firmware — only the SD card contents differ. Adding a new config field means: add to `struct device_config`, add a default in `device_config_defaults`, add a `parse_line` branch, and pass the value where needed.

### Devicetree

The overlay is `boards/adafruit_feather_nrf52840_nrf52840_uf2.overlay`. Application-specific GPIOs live under the `zephyr,user` node. Sensor/peripheral nodes go under their bus (`&i2c0`, `&spi1`).

## Testing

Tests use GoogleTest (C++), wrapping C code under test with `extern "C"`. Test files go in `tests/` as `*_test.cpp`. The CMakeLists auto-discovers test files and links the corresponding `src/*.c` module.


## Design Principles

- **Modules depend on abstractions, not each other.** A producer pushes events into a `k_msgq` — it never calls into the audio subsystem, and it doesn't know who consumes the events. New modules should follow this: communicate through the event queue or a narrow init-time interface, never by reaching into another module's state.
- **Every module owns its state.** All module state is `static`. No module exposes its internal variables — only functions. If two modules need shared data, one owns it and the other asks for it through a function call. Init functions must explicitly initialize all module state — never rely on startup code zeroing globals.
- **Dependencies flow one direction.** `main()` knows about every module; modules don't know about `main()` or each other. The call graph should always be a tree rooted at `main()`, never a cycle.
- **Solve the problem in front of you.** Don't add configuration for something that has exactly one correct value. Don't add an abstraction layer for something that has exactly one implementation. Don't add a feature that nobody has asked for yet.
- **Eliminate duplication only when the duplicates must change together.** Three similar lines that serve different purposes are better than a shared helper that couples unrelated callers. Extract only when a change to one copy would always require the same change to the others.
- **Make it work, then make it right.** A working solution with rough edges beats an elegant solution that doesn't compile. Ship the simple version first; refactor when the simple version causes a real problem, not a hypothetical one.

## C Code Style

Indentation is **4-wide spaces, not tabs**. Otherwise follow the [Zephyr coding style](https://docs.zephyrproject.org/latest/contribute/style/index.html) (Linux-kernel derived): function opening brace on its own line, control-statement brace on the same line.

- Never use `goto` — extract helper functions instead
- Prefer `for (;;)` over `while (1)`
- All file-scope `static` variables are prefixed with `g_`, including `const` hardware descriptors
- All file-scope `static` variables and Zephyr kernel object definitions (`K_SEM_DEFINE`, `K_THREAD_DEFINE`, etc.) must be collected near the top of the file, before any function definitions. Add forward declarations as needed. If this pushes globals far from the functions that use them, split the module.
- Besides `g_`, no other type or storage prefixes should be used (no Hungarian notation)
- Header guards: `#ifndef FILENAME_H_` / `#define FILENAME_H_` / `#endif /* FILENAME_H_ */`
- Includes ordered: Zephyr headers, stdlib headers, local headers (blank line between groups)
- Every `.c` registers a log module: `LOG_MODULE_REGISTER(name, LOG_LEVEL_INF)`
- Error handling: return negative errno, check with `if (ret)` and early return
- Public functions prefixed with module name: `accel_init()`, `audio_play_sound()`
- Static helpers use descriptive names without module prefix
- Structs and enums are lowercase `snake_case`, no typedefs
- Macros are `ALL_CAPS`
- Shell commands registered at file scope with `SHELL_CMD_ARG_REGISTER`
