# Changelog

## v1.0 — Terminal UI & editability pass

- Redesigned the main screen with a five-row ASCII timer, color-coded states, theme colors, clearer section separators, and a more informative keyboard legend.
- Added automatic `~/.nexuscli/config.conf` creation, validation, atomic updates, a settings menu (`C`), and `--config`.
- Made inspection limits, hold threshold, scramble length, recent-row count, theme, and colors configurable.
- Added an in-app help guide (`?`) plus `--version` and expanded `--help` output.
- Reworked solve editing (`M`) to select any stored solve and edit raw time, penalty, session assignment, bookmark state, or scramble; it can also delete the selected record.
- Added an explicit-confirmation delete shortcut (`X`) for the latest solve.
- Added a bookmarked-solves view (`K`), a 20-solve ASCII trend chart (`P`), and custom/pasted current scrambles (`R`).
- Added session switching (`G`) and made new sessions use an ID above the highest recorded session, even after switching to an older session.
- Persisted the selected session across launches, including empty sessions.
- Distinguished current session averages, session-best averages, current all-record averages, and all-time best averages in the dashboard.
- Kept the v0.4 solve file and backward compatibility with older rows; no migration step is required.
- Preserved and retained the CSV export improvements from v0.4.

## Limitations carried forward

- Ordinary terminal input cannot provide reliable exact key-up timing for Space; start remains a key-repeat heuristic.
- Scrambles are random-move sequences, not official state-based WCA scrambles.
