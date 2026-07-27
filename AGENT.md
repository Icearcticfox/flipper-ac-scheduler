# Agent Notes

This project is a Flipper Zero external FAP application built with uFBT.

## Project Rules

- Work inside this directory: `ac_scheduler`.
- Keep the app ID as `ac_scheduler`.
- Keep the entry point as `ac_scheduler_app`.
- Do not add backup files or generated source copies.
- Do not commit build output from `dist/`.
- Keep application source files in `src/`.
- Keep `ac_config.h` in the project root so users can edit saved remote settings easily.
- Use `apply_patch` for manual code edits.
- Run `ufbt` before reporting completion.

## Important Files

- `application.fam`: FAP manifest.
- `ac_config.h`: saved IR remote path, button names, debug flag, default schedule.
- `src/ac_scheduler.c`: app lifecycle, GUI, input, settings persistence.
- `src/ac_schedule.c`: schedule calculation; keep GUI-independent.
- `src/ac_infrared.c`: saved remote loading and IR transmission; keep SDK-specific IR logic isolated here.

## Build

```sh
ufbt
```

Fallback from the current local workspace:

```sh
../.venv/bin/ufbt
```

Expected output:

```text
dist/ac_scheduler.fap
```

## Runtime Notes

- IR remote: `/ext/infrared/Remote.ir`.
- ON button: `Ac_on_cool_23_2`.
- OFF button: `Ac_off`.
- Runtime settings are stored on Flipper SD at `/ext/apps_data/ac_scheduler/settings.bin`.
- Reinstalling the FAP should not overwrite runtime settings.
- The app does not run in the background after exit.
