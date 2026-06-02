# Departure Planner — Feature Overview

**Author:** Patrick Gorman  
**Date:** 2026-06-01  
**Status:** Design complete; implementation complete  
**Detailed spec:** `docs/DEPARTURE_SWEEP_CONTRACT_FINAL-1.md`  
**Design framework:** `docs/DEPARTURE_PLANNING.md`

---

## What it does

The Departure Planner answers the question every offshore sailor faces before a passage: *which departure time gives me the best trip?*

It sweeps a range of candidate departure times across the loaded GRIB window, runs the existing WRPI isochrone engine for each candidate, and presents the results ranked by the user's chosen objective — fastest arrival, smoothest conditions, or a weighted balance of both. The user can apply a chosen departure time to the active route configuration in a single click.

This is a natural extension of WRPI's existing **Batch** feature (`GenerateBatch`). Where Batch varies configuration parameters, the Departure Planner varies `StartTime` across a user-defined window and focuses the output on the single decision: *when do I leave?*

---

## Value to users

- **No manual guesswork.** Instead of manually editing `StartTime` and re-running routing one departure at a time, the planner evaluates dozens of candidates automatically.
- **Honest comparison.** Results show route duration, average and peak wind speed, maximum swell height, and a personal comfort penalty side by side so the user can see the trade-off clearly.
- **Arrival constraints.** Optional arrival window filter deprioritises candidates that would arrive at the wrong time of day — useful for scheduled deliveries, tide-dependent harbours, or avoiding a night landfall. Mode A specifies a fixed time-of-day window; Mode B pins arrival before sunset at the destination (with an optional buffer margin).
- **Personal comfort scoring.** WRPI can load an exported WindTrack comfort profile and use it to score each candidate's route against the owner's actual comfort threshold — not a generic wind speed proxy. If no profile is loaded, WRPI falls back to average TWS as a comfort proxy automatically.

---

## How it fits inside WRPI

The feature reuses the existing routing engine unchanged.

```
DeparturePlanningDialog (WR.3)
        |
        v
DepartureSweepController (WR.1 + WR.2)
   |-- clones RouteMapConfiguration with StartTime = tᵢ
   |-- creates RouteMapOverlay for each candidate
   |-- dispatches up to N concurrent RouteMapOverlayThread instances
   |-- collects results into SweepCandidate list
   |-- calls ranking engine (WR.2)
        |
        v
Results table  -->  Apply (sets StartTime on base config)
               -->  Inspect (opens StatisticsDialog + PlotDialog on candidate overlay)
```

No changes to `RouteMap`, `IsoRoute`, `Position`, or any routing algorithm. The planner is additive — it introduces new files and adds one button to the WRPI main dialog.

---

## New source files

| File | Deliverable | Description |
|------|------------|-------------|
| `src/DepartureSweepController.h/cpp` | WR.1 + WR.2 | Sweep queue, thread throttle, result collection, ranking engine. `RouteMapOverlay` objects are freed immediately after stats extraction to conserve 32-bit address space; lightweight `SweepCandidate` structs are retained for re-sort, re-filter, and Inspect. Inspect triggers a single-candidate on-demand recompute. No wx dependency beyond `wxDateTime`. |
| `src/DeparturePlanningDialog.h/cpp` | WR.3 | Non-modal wx dialog. Communicates with `DepartureSweepController` via `wxCallAfter`. |
| `src/ComfortProfileLoader.h/cpp` | WR.4 | Load, validate, and score a WindTrack comfort profile JSON file against a `PlotData` chain. No wx; uses jsoncpp (already a WRPI dependency). |
| `src/SolarCalculator.h/cpp` | §3.6.2 | Standalone sunset calculation (NOAA algorithm, ±5 min accuracy at latitudes below 70°). No wx, no OpenCPN API. Fully unit-testable. |

### Modifications to existing files

| File | Change |
|------|--------|
| `src/WeatherRouting.h` | Add `void OpenDeparturePlanning()` and `DeparturePlanningDialog* m_departurePlanningDialog`. |
| `src/WeatherRouting.cpp` | Add "Departure Planning…" button; implement `OpenDeparturePlanning()`; free dialog in destructor. |
| `src/weather_routing_pi.cpp` | Wire `GRIB_TIMELINE` availability to `DeparturePlanningDialog::OnGribTimelineAvailable()` when dialog is open. |

---

## Sweep algorithm

1. Clamp the user's departure window to the loaded GRIB valid range. Warn if clamping is needed; abort if the window collapses.
2. Generate candidate departure times at the chosen step (default 3 h; options: 1 h / 2 h / 3 h / 6 h / 12 h / 24 h / GRIB spacing). Step size does not need to align to GRIB record boundaries — `WeatherDataProvider` interpolates transparently between records at every isochrone step, so a 9:00 departure on a 6-hour GRIB is fully valid and may genuinely be the best choice.
3. For each candidate: clone the base `RouteMapConfiguration` with `StartTime = tᵢ`, create a `RouteMapOverlay`, dispatch to `RouteMapOverlayThread`. Throttle to at most N concurrent threads (default 4).
4. After each thread completes, collect a `SweepCandidate`: duration, avg/max TWS, max swell, comfort penalty, arrival filter result.
5. When all candidates finish (or the user cancels), rank and display.

Cancellation is clean: pending candidates are not started; running threads are stopped via the existing `RouteMapOverlay::Stop()` mechanism; partial results are ranked and shown with a banner.

---

## Ranking

Three sort modes:

| Mode | Sort key |
|------|---------|
| **FASTEST** | Shortest duration |
| **SMOOTHEST** | Lowest comfort penalty (or `avg_tws_kn` if no profile loaded) |
| **BALANCED** | `duration_h × (1 + w × score)`; user-adjustable weight w in [0, 1] |

Results are divided into three tiers displayed in order:

1. **Succeeded, arrival window satisfied** — normal styling
2. **Succeeded, arrival window missed** — italicised (deprioritised but never hidden; user may still Apply)
3. **Failed** — dimmed with failure reason

Re-ranking (changing mode, weight, or arrival filter) re-sorts the in-memory list instantly — no re-run needed.

---

## Arrival window filter

Optional. Two modes, selectable per session:

**Fixed time-of-day window** — the user specifies earliest and latest arrival times in the display timezone. Handles wrap-midnight windows (e.g. 20:00–06:00).

**Before sunset at destination** — uses `SolarCalculator::Sunset()` to compute sunset at the route's end coordinates on the ETA date. Optional buffer margin (hours before sunset). Polar edge cases: midnight sun → always `arrival_ok`; polar night → always `arrival_ok = false` with an explanatory message.

---

## WindTrack comfort profile integration

WindTrack (a separate OpenCPN plugin) exports personal comfort profiles in a versioned JSON format. Schema v1 is frozen (2026-05-20; `docs/COMFORT_PROFILE.md` in the WindTrack repo).

The profile declares a `threshold_bins` model: two bin edges and three penalty weights per feature. Features in the vocabulary: `tws_kn`, `twa_deg`, `wvht_m`, `wvper_s`. Wind-only (2-feature) and wave-augmented (4-feature) profiles are both supported; the `input_features` list is self-describing.

`ComfortProfileLoader` validates the file on load and scores each candidate's `PlotData` chain:

1. Walk waypoints; extract TWS, TWA, and (if present in GRIB and profile) wave height and period.
2. For each waypoint, look up the bin index for each feature and read `penalties[bin_index]`.
3. `comfort_penalty = mean over waypoints of mean over features`.

If the profile is absent, invalid, or expired, WRPI falls back to `avg_tws_kn` as the comfort proxy and indicates this in the status label. No crash, no silent wrong scoring.

The profile file path, ranking mode, balanced weight, and display timezone offset are all persisted in wx config under `/PlugIns/WeatherRouting/DeparturePlanning/`.

---

## Comfort score — practical interpretation

The `comfort_penalty` column is a number in [0, 1] computed as the time-weighted mean of
per-waypoint penalties across the route. Each waypoint contributes one penalty per active
feature (TWS, wave height, wave period, TWA), and the result is normalised by the number of
features. It is **a passage average, not a worst-case value** — a short brutal stretch is
smoothed by easy miles before and after it.

### The cardinal rule: relative, not absolute

> **The comfort score is meaningful in relative terms, not absolute terms. Use it to rank
> candidates against each other — not to judge the passage itself.**

The absolute number is entirely dependent on how the comfort profile was configured. A skipper
who set TWS thresholds at 15/20 kn will see scores of 0.6+ on conditions that a profile
calibrated at 28/30 kn scores at 0.22. The same weather window, the same route, wildly
different numbers. Neither is wrong — they reflect different skippers' comfort envelopes.

Within a single sweep — same profile, same departure window — a lower score is always a better
candidate. That ranking is stable and trustworthy regardless of how the profile is calibrated.
The absolute number is not portable across profiles or between users.

### The right workflow

1. **Sort by comfort score** to surface the best candidates from the sweep.
2. **Inspect the short list in WRPI** to get the per-waypoint Good / Bumpy / Difficult
   assessment before committing. WRPI's label is profile-independent and is the portable
   ground truth for judging whether a passage is actually acceptable.

The comfort score narrows the field. WRPI blesses the choice.

### Example score ranges (profile-specific)

The ranges below were observed on a Jeanneau SO49DS with TWS thresholds set near gale force
(28/28.5 kn), so wind speed contributes minimally and the score is driven by wave height,
wave period, and point of sail. **These ranges will not transfer to a differently calibrated
profile** — treat them as illustrative, not universal.

| Score | Passage character | WRPI typically showed |
|-------|------------------|-----------------------|
| < 0.15 | Running or reaching in light air, long swell | Good |
| 0.15 – 0.25 | Trades-style: reaching, 1.5–2 m swell, decent period | Good to Bumpy |
| 0.25 – 0.35 | Mixed: some beating, or shorter wave period | Bumpy |
| 0.35 – 0.50 | Sustained beat into chop, or heavy swell | Bumpy to Difficult |
| > 0.50 | Multiple bad factors combined — rough passage | Difficult |

The WRPI label and the comfort score are **complementary, not equivalent.** WRPI's
`sailingConditionLevel()` uses apparent wind angle and takes the worst single waypoint as the
route label — one rough stretch makes the whole route "Bumpy." The comfort score averages over
the full passage and uses true wind angle. A route that is mostly easy with one hard overnight
beat may score 0.28 (Bumpy boundary) while WRPI calls it "Bumpy" due to that one stretch.

### Known limitation (KL-1)

The comfort score does not model bow-slamming on a close-hauled beat into short-period chop.
The wave period penalty addresses this partially, but the model lacks the apparent-wind-angle
term that WRPI uses to amplify discomfort at ~35° off the bow. As wave-augmented passage data
accumulates, the comfort profile will learn Shanti's actual motion response and the gap will
narrow. Until then, WRPI's route-level label is the more reliable indicator for passages with
significant upwind miles.

---

## Results table columns

| Column | Content |
|--------|---------|
| Rank | 1-based across all succeeded candidates |
| Departure | Date/time in selected display timezone |
| ETA | Date/time in selected display timezone |
| Duration | `Xd HH:MM` |
| Avg TWS (kn) | Mean wind speed along route |
| Max TWS (kn) | Peak wind speed encountered |
| Max Swell (m) | Peak significant wave height; "n/a" if no swell in GRIB |
| Comfort | `comfort_penalty` or "n/a" |
| Arrival | Check mark / miss reason / hidden when filter disabled |
| Status | "OK" or failure reason |

---

## Inspect action

Clicking **Inspect** on a succeeded candidate triggers an on-demand single-candidate recompute: the controller clones the base configuration with that candidate's departure time and runs the routing engine for that one departure. The **Inspect** button label changes to **Inspecting...** during the recompute (typically 5–30 seconds depending on route length and GRIB density). On completion, WRPI's existing `StatisticsDialog` and `PlotDialog` are opened against the freshly computed overlay. No new dialog classes required. The candidate overlay does not appear in the main route list. Closing the Departure Planning dialog frees the inspect overlay.

**Apply** copies the candidate's `departure_utc` into the base configuration's `StartTime` and does nothing else — the user then runs the route manually from the main WRPI dialog as normal.

---

## Unit tests

18 unit tests covering: ranking (all three modes), three-tier ordering, balanced weight monotonicity, comfort profile load/validate/score (valid and six invalid variants), arrival filter (fixed window including wrap-midnight, before-sunset, polar edge cases), solar calculator against NOAA reference values (three lat/lon/date points, ±5 min tolerance), display timezone formatting (UTC offset applied to departure/ETA display and miss-reason strings), peak stats extraction from `PlotData` chain. All tests run without an OpenCPN runtime.

All existing WRPI unit tests continue to pass — no routing algorithm is modified.

---

## Current status

| Item | State |
|------|-------|
| Design framework (`DEPARTURE_PLANNING.md`) | Complete |
| Implementation contract (`DEPARTURE_SWEEP_CONTRACT_FINAL-1.md`) | Frozen 2026-05-21 |
| WindTrack comfort profile export (WindTrack side) | Complete and tested |
| WRPI implementation (new source files above) | Complete |
| Visual verification in OpenCPN | Confirmed working |

Both sides of the integration are production-ready. The comfort profile format is frozen; the WindTrack export/load/validate pipeline is fully tested. The WRPI implementation is complete and has been verified end-to-end in OpenCPN.

---

## Post-release refinements (2026-06-01)

Three improvements delivered after initial visual verification, addressing issues found during extended use:

| Commit | Change |
|--------|--------|
| `fab1f21` | **Overlay memory optimization** — `RouteMapOverlay` objects freed immediately after stats extraction rather than retained until dialog close. Inspect recomputes on demand. Eliminates address-space pressure (OpenCPN is a 32-bit process) when sweeping large candidate sets. |
| `9fdca95` | **Cancel completion bug** — After cancelling a mid-sweep run, `IsRunning()` would remain true and the Run button would stay disabled, requiring an OCPN restart to recover. Fixed by including the cancelled state in the sweep-complete condition. |
| `c1f3113` | **Inspect progress indicator** — The Inspect button now shows "Inspecting..." and disables during the on-demand recompute so users know the engine is working. |

---

## References

- Full implementation contract (data model, invariants, exit criteria): `docs/DEPARTURE_SWEEP_CONTRACT_FINAL-1.md`
- Design rationale, phasing, and WindTrack integration requirements: `docs/DEPARTURE_PLANNING.md`
- WindTrack comfort profile schema: `docs/COMFORT_PROFILE.md` in the WindTrack repo
