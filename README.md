# NexusCLI C v1.0— Terminal Speedcubing Lab

A local-only, terminal-based speedcubing timer written in C. Version 1.0 focuses on a much more expressive TUI, configurable behavior, editable solve records, and clearer statistics while keeping the runtime dependency footprint small.

NexusCLI is an independent community project inspired by common speedcubing timer and statistics workflows. It is not an official NexusTimer release and contains no copied NexusTimer source code.

## Build on Termux

Unzip the package, enter the extracted directory, then compile:

```sh
pkg install clang
clang -O2 -Wall -Wextra -Wpedantic -std=c11 nexuscli.c -lm -o nexuscli
./nexuscli
```

The executable is compiled for the device and operating system where you build it. Keep `nexuscli.c` if you may want to rebuild later. Clang is not needed just to run an already-built executable.

## Interface and controls

The main screen includes a large five-row ASCII timer, color themes, separate session/all-time statistics, a scramble panel, recent solves, and a keyboard legend. The display is ASCII-based to avoid relying on special box-drawing fonts. Set `colors=0` or use the `NO_COLOR` environment variable to disable color escape sequences.

| Key | Action |
|---|---|
| Space | Hold until the configured threshold, then release to start; Space while running stops and saves |
| `I` | Start inspection |
| `N` | Generate a new random-move scramble |
| `R` | Paste/customize the current scramble (useful for externally generated scrambles) |
| `S` | Create a new session ID after the highest existing session |
| `G` | Switch to an existing session |
| `+` or `=` | Toggle +2 for the latest solve |
| `D` | Toggle DNF for the latest solve |
| `B` | Bookmark/unbookmark the latest solve |
| `M` | Open the solve editor; select a record and edit its time, penalty, session, bookmark, or scramble, or delete it |
| `X` | Delete the latest solve after typing `yes` |
| `U` | Undo/remove the latest solve |
| `H` | Recent solve history |
| `K` | List bookmarked solves |
| `V` | Session overview |
| `A` | Seven-day activity chart |
| `P` | ASCII time-trend chart for up to 20 recent solves |
| `E` | Export all solves to CSV |
| `C` | Settings menu |
| `?` | In-app help guide |
| `Q` | Save and quit |

The solve editor uses **raw time** for edits. Enter a time as seconds (for example `27.91`) or `M:SS.cc` (for example `1:02.45`). Penalties are edited separately. Changing a solve's raw time does not alter its original solve date.

Inspection defaults are 15 seconds for no penalty and over 15 through 17 seconds for +2; past 17 seconds is DNF. These thresholds can be adjusted in settings. Inspection begins manually with `I`.

## Configuration

On first launch the program creates:

```text
~/.nexuscli/config.conf
```

Press `C` inside the app to change supported settings, or edit that file directly. Supported settings are:

- `colors`: `0` or `1`
- `theme`: `cyan`, `green`, `amber`, `blue`, or `magenta`
- `inspection_seconds`: boundary between no penalty and +2
- `inspection_dnf_seconds`: boundary after which inspection incurs DNF; must exceed `inspection_seconds`
- `hold_ms`: minimum hold-to-start threshold (250–1500 ms)
- `scramble_length`: random-move sequence length (10–40)
- `recent_solves`: number of recent rows displayed (0–10)
- `current_session_id`: last-selected session, persisted between launches

The configuration file is written through a temporary file and rename to reduce the risk of leaving a partially written configuration. Unknown configuration keys are not currently used by the app and may be dropped when the app saves settings.

## Data and CSV export

Solve history stays in the same file used by v0.4:

```text
~/.nexuscli/solves.tsv
```

This release continues to read older v0.1/v0.2-style rows and the v0.4 data format. Back up the entire `~/.nexuscli` directory before experimenting with record editing or deletion.

Press `E` in the app or run:

```sh
./nexuscli --export-csv
```

The export first tries `~/storage/shared/Download/nexuscli-solves.csv`, if Termux shared storage is available and writable; otherwise it falls back to `~/.nexuscli/solves.csv`. The full output path is printed. CSV fields are escaped, and write/flush/close errors are checked.

Additional terminal commands:

```sh
./nexuscli --help
./nexuscli --version
./nexuscli --config
```

## Statistics

The TUI distinguishes current averages from best averages. Session statistics are scoped to the selected session; all-time statistics aggregate the recorded solve list. Ao3 is the arithmetic mean of the latest three solves; Ao5/Ao12 trim the fastest and slowest result for each window. DNFs count as the worst result, so one DNF can be trimmed where appropriate but a DNF remaining in the average makes that average DNF.

## Known limitations

- **Keyboard key-up:** POSIX terminal input normally does not expose an exact Space key-release event. Hold-to-start uses keyboard-repeat timing, with a configurable minimum threshold and a repeat-settle delay. Start timing can vary by keyboard and terminal configuration, so it is not as reliable as a dedicated physical timer pad.
- **Scramble validity:** The included generator creates random-move sequences and avoids repeating the same face immediately. It is not a state-based or WCA-valid scramble generator.
- **Scope:** This is a local 3x3-style terminal tool. It does not include smart-cube hardware, cube inventories, accounts, online sync, multiplayer, algorithm trainers, or solve replay.
- **Terminal:** Designed for Termux/Linux-like POSIX terminals with `termios`, `select`, ANSI cursor movement, and `clock_gettime`.

## Validation

For this build, both GCC and Clang compiled the program with `-Wall -Wextra -Wpedantic` and no warnings. Local checks covered legacy solve loading, configuration persistence, CSV quoting/export, the in-app settings menu, editing a solve time, and clean terminal restoration on quit. AddressSanitizer/UndefinedBehaviorSanitizer was run on the CLI export path. These are build-environment tests; the package still needs testing on the actual TV box and its keyboard.
