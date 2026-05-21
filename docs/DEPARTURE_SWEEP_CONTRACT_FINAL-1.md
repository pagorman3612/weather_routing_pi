# WeatherRouting_Pi — Departure Sweep Contract

**Status:** FINAL-1  
**Date:** 2026-05-21  
**Design authority:** `docs/DEPARTURE_PLANNING.md` — goals, phasing rationale, and WindTrack
comfort profile integration requirements are defined there. This contract adds the precision
required for implementation: data model, code touchpoints, invariants, failure modes, and
exit criteria.

---

## 1. Purpose

Deliver a departure-time planning feature inside WeatherRouting_Pi that:

1. Steps candidate departure times across the loaded GRIB window.
2. Runs the existing isochrone engine (`RouteMapOverlay`) for each candidate.
3. Ranks results by duration and/or comfort.
4. Optionally applies a WindTrack-exported comfort profile as a personal scoring layer.
5. Lets the user apply a chosen departure time to the active route configuration.

This is an **internal WRPI feature**. It does not require a plugin messaging API with
WindTrack. The only external integration is a file-based comfort profile (§4).

Reference: `docs/DEPARTURE_PLANNING.md` §2 (fit inside WRPI), §3 (constraints/preferences),
§5 (phased delivery), §7 (WindTrack comfort profile).

---

## 2. Deliverables

| ID | Deliverable | Maps to `DEPARTURE_PLANNING.md` |
|----|-------------|----------------------------------|
| WR.1 | Sweep orchestration engine | Phase B |
| WR.2 | Ranking engine | Phase A + C |
| WR.3 | Departure Planning Dialog (UI) | Phase C |
| WR.4 | WindTrack comfort profile loader | Phase D / §7 |

WR.1 and WR.2 are the engine core and may be tested independently. WR.3 and WR.4 depend on
WR.1+WR.2.

---

## 3. WR.1 — Sweep Orchestration Engine

### 3.1 Inputs

| Parameter | Type | Source |
|-----------|------|--------|
| Base configuration | `RouteMapConfiguration` | The currently-selected `RouteMapOverlay` in WRPI's route list. Start, End, Boat, constraints, and `DeltaTime` are all inherited. |
| `sweep_start` | `wxDateTime` (UTC) | User input in `DeparturePlanningDialog`, entered in display timezone and converted to UTC on input. |
| `sweep_end` | `wxDateTime` (UTC) | Same. SHALL satisfy `sweep_end > sweep_start`. |
| `step_s` | `int` (seconds) | Departure step size; default = 3 h (10800 s); offered options 1 h / 2 h / 3 h / 6 h / 12 h / 24 h / GRIB spacing (auto); minimum = `DeltaTime`. Step size need not align to GRIB record boundaries — see §3.2 note. |
| `max_concurrent` | `int` | Maximum simultaneous `RouteMapOverlayThread` instances; default = 4; configurable. |
| `display_tz_offset_min` | `int` (minutes) | UTC offset for display and input, range −720 to +840. 0 = UTC. Display only — does not affect computation. |

### 3.2 Algorithm

1. **Clamp window to GRIB valid range.** Request `GRIB_TIMELINE` if not already available.
   If `sweep_start` lies before GRIB start, advance to GRIB start and warn user.
   If `sweep_end` lies beyond GRIB end, clamp and warn user.
   If window collapses to zero after clamping, report error and abort.

2. **Build candidate list.** Generate departure times
   `t₀ = sweep_start`, `t₁ = t₀ + step_s`, `t₂ = t₀ + 2×step_s`, …
   up to and including `sweep_end`. All times are UTC.

   > **GRIB interpolation note.** Candidate departure times do not need to coincide with
   > GRIB record boundaries. `WeatherDataProvider` performs linear temporal interpolation
   > between adjacent GRIB records at every isochrone propagation step regardless of where
   > the departure falls. A 9 am departure on a 6-hour GRIB (records at 0 h, 6 h, 12 h, …)
   > is fully valid — its route is computed against weather interpolated between the 6 am
   > and 12 pm records. This is the same mechanism used by all WRPI routing, not special
   > handling for the sweep. Finer step sizes surface departure candidates that coarser
   > steps would never evaluate, and those candidates can be genuinely better choices.

3. **For each candidate time `tᵢ`:**
   Clone the base `RouteMapConfiguration` with `StartTime = tᵢ`.
   Create a `RouteMapOverlay` and call `SetConfiguration()`.
   Call `LoadBoat()`.
   Add to the compute queue.

4. **Run queue with throttle.** Dispatch up to `max_concurrent` overlays at once via the
   existing `RouteMapOverlayThread`. As each thread finishes (success or error), dispatch
   the next from the queue and collect the result into a `SweepCandidate` (§3.4).

5. **Completion.** When the queue is empty and all threads have joined, invoke WR.2
   to rank the results and notify the dialog.

### 3.3 Cancellation

The user may press Cancel at any time.

- Pending configurations in the queue SHALL NOT be started.
- Running `RouteMapOverlay` threads SHALL be stopped via the existing
  `RouteMapOverlay::Stop()` mechanism.
- Partial results collected so far SHALL be ranked and displayed; a banner SHALL indicate
  the sweep was cancelled.

### 3.4 `SweepCandidate` data model

```cpp
struct SweepCandidate {
    wxDateTime  departure_utc;
    wxDateTime  eta_utc;            // RouteMapOverlay::EndTime()
    wxTimeSpan  duration;           // eta_utc - departure_utc
    bool        succeeded;          // Finished() && !GotError()
    wxString    failure_reason;     // non-empty when !succeeded
    double      avg_tws_kn;         // mean TWS along PlotData chain; NaN if !succeeded
    double      max_tws_kn;         // peak TWS encountered along route; NaN if !succeeded
    double      max_swell_m;        // peak significant wave height along route; NaN if no swell data or !succeeded
    double      upwind_fraction;    // fraction of waypoints where TWA < 60°; NaN if !succeeded
    double      comfort_penalty;    // [0,1] from WindTrack profile; NaN if no profile loaded
    bool        arrival_ok;         // true if ETA satisfies arrival window, or filter disabled
    wxString    arrival_miss;       // human-readable reason when !arrival_ok; empty otherwise
    int         rank;               // 1-based among succeeded candidates; 0 = failed
};
```

`avg_tws_kn`, `max_tws_kn`, `max_swell_m`, and `upwind_fraction` are sampled by walking
the `RouteMapOverlay`'s `PlotData` chain after the run completes. `max_swell_m` is NaN
when the loaded GRIB contains no swell data. The `RouteMapOverlay` object is retained in
memory by `DepartureSweepController` for the lifetime of `DeparturePlanningDialog` to
support the Inspect action (§5.5); it is freed when the dialog closes.

`arrival_ok` is evaluated by WR.1 immediately after each candidate's thread finishes,
using the arrival window parameters supplied to the sweep (§3.6). When the arrival window
filter is disabled, `arrival_ok = true` for all succeeded candidates.

### 3.6 Arrival Window Filter

The arrival window filter is **optional** (controlled by a checkbox in the dialog).
When disabled, `arrival_ok = true` for all succeeded candidates and ranking is unaffected.

#### 3.6.1 Filter modes

Two mutually-exclusive modes, selected by the user:

**Mode A — Fixed time-of-day window**

The user specifies `arrival_earliest` and `arrival_latest` as time-of-day values entered
in the display timezone. Internally they are stored as UTC time-of-day (stored value =
entered value − `display_tz_offset_min`). Evaluation always uses UTC.

- If `arrival_earliest ≤ arrival_latest` (UTC): same-day window. `arrival_ok` when
  `arrival_earliest ≤ eta_tod ≤ arrival_latest` where `eta_tod` is the UTC time-of-day of `eta_utc`.
- If `arrival_earliest > arrival_latest` (UTC): window wraps midnight (e.g. 20:00–06:00 UTC).
  `arrival_ok` when `eta_tod ≥ arrival_earliest` OR `eta_tod ≤ arrival_latest`.

When the user changes `display_tz_offset_min`, the stored UTC values are recalculated from
the displayed values; the logical constraint is unchanged.

When `!arrival_ok`, `arrival_miss` SHALL read (using display timezone):
`"Arrives HH:MM <tz> — outside window HH:MM–HH:MM <tz>"`
where `<tz>` is `"UTC"` when offset = 0, otherwise `"UTC+H:MM"` or `"UTC−H:MM"`.

**Mode B — Before sunset at destination**

The user optionally specifies a margin `sunset_margin_h ≥ 0` (hours before sunset; default 0).
`arrival_ok` when `eta_utc ≤ sunset_utc(eta_date, end_lat, end_lon) − sunset_margin_h`.

`end_lat` and `end_lon` are taken from the base `RouteMapConfiguration`.
`sunset_utc` is computed by `SolarCalculator::Sunset()` (§3.6.2).

Special cases:
- **Midnight sun** (sun never sets on `eta_date` at destination): `arrival_ok = true` regardless of time.
- **Polar night** (sun never rises on `eta_date`): `arrival_ok = false`; `arrival_miss` reads
  `"Polar night at destination on ETA date"`.

When `!arrival_ok` (normal case), `arrival_miss` SHALL read:
`"Arrives HH:MM <tz> — sunset HH:MM <tz>"` (including margin in the displayed sunset time
when `sunset_margin_h > 0`, e.g. `"sunset (−2 h buffer) HH:MM <tz>"`).
`<tz>` uses the same label convention as Mode A.

#### 3.6.2 Solar calculation

`SolarCalculator` is a new standalone utility class (`src/SolarCalculator.h/cpp`).
It SHALL have no wx dependency and no OpenCPN API dependency so that it is fully unit-testable.

```cpp
class SolarCalculator {
public:
    // Returns UTC time of sunset for the given date at (lat, lon).
    // Returns wxInvalidDateTime if sun never sets (midnight sun).
    // Returns wxDateTime with IsValid() == false and sets polar_night = true
    // if sun never rises.
    static wxDateTime Sunset(int year, int month, int day,
                             double lat_deg, double lon_deg,
                             bool* polar_night = nullptr);
};
```

Algorithm: NOAA solar position algorithm (Jean Meeus, *Astronomical Algorithms*, 2nd ed.,
or equivalent). Required accuracy: ±5 minutes at latitudes below 70°. Sunset is defined
as the moment the upper limb of the sun crosses the horizon corrected for standard
atmospheric refraction (solar altitude = −0.833°).

#### 3.6.3 Inputs to the filter

| Parameter | Type | Default |
|-----------|------|---------|
| `arrival_filter_enabled` | `bool` | `false` |
| `arrival_filter_mode` | enum `FIXED_WINDOW` / `BEFORE_SUNSET` | `BEFORE_SUNSET` |
| `arrival_earliest` | minutes from midnight, UTC | `360` (06:00 UTC) (Mode A only) |
| `arrival_latest` | minutes from midnight, UTC | `1200` (20:00 UTC) (Mode A only) |
| `sunset_margin_h` | `double ≥ 0` | `0.0` (Mode B only) |
| `display_tz_offset_min` | `int`, −720 to +840 | `0` (UTC) |

All parameters are persisted in wx config under
`/PlugIns/WeatherRouting/DeparturePlanning/`
(`ArrivalFilter/` sub-key for arrival filter params; `DisplayTzOffsetMin` at top level).

### 3.7 Progress reporting

The dialog SHALL show a progress indicator: `N of M candidates complete` (updated on each
thread completion, from the UI thread via `wxCallAfter` or equivalent). Time elapsed SHALL
be shown.

---

## 4. WR.2 — Ranking Engine

### 4.1 Sort modes

| Mode | Sort key (ascending) | Description |
|------|---------------------|-------------|
| `FASTEST` | `duration` | Shortest passage time first. |
| `SMOOTHEST` | `comfort_penalty` (profile loaded) or `avg_tws_kn` (no profile) | Lightest-air / lowest-discomfort first. |
| `BALANCED` | `duration_h × (1 + w × score)` where `score = comfort_penalty` or `avg_tws_kn / 30` | Weighted trade-off; default weight `w = 0.5`. |

Default mode: `BALANCED`.

`BALANCED` weight `w` SHALL be user-configurable (slider or spin control, range 0.0–1.0,
step 0.1, persisted in wx config). `w = 0` collapses to `FASTEST`; `w = 1` emphasises
comfort more strongly.

### 4.2 Ranking rules

Candidates are sorted into **three tiers** in strict top-to-bottom order:

| Tier | Condition | Within-tier sort |
|------|-----------|-----------------|
| 1 | `succeeded && arrival_ok` | By selected sort mode (ascending) |
| 2 | `succeeded && !arrival_ok` | By selected sort mode (ascending) |
| 3 | `!succeeded` | By `departure_utc` ascending |

Additional rules:

1. Tier ordering is absolute: any Tier 1 candidate ranks above any Tier 2 candidate,
   which ranks above any Tier 3 candidate, regardless of sort mode.
2. Rank is 1-based across **all** succeeded candidates (Tier 1 + Tier 2 combined),
   so the user sees a continuous rank sequence and can compare absolute quality.
   Failed candidates (`rank = 0`) are not numbered.
3. When the arrival window filter is disabled, all succeeded candidates are Tier 1
   and the three-tier structure collapses to the original two-tier (succeeded / failed).
4. Re-ranking on mode, weight, or arrival filter change SHALL be instantaneous
   (re-sort in memory only; no re-running the engine).

### 4.3 Comfort proxy (no profile)

When no WindTrack profile is loaded, `SMOOTHEST` and `BALANCED` use `avg_tws_kn` as a
proxy for discomfort. This is documented in the UI ("comfort estimate — no personal profile
loaded").

---

## 5. WR.3 — Departure Planning Dialog

### 5.1 Access

A **"Departure Planning…"** button SHALL be added to the `WeatherRouting` main dialog
toolbar or button strip. It opens `DeparturePlanningDialog` as a non-modal `wxDialog`
(allows interaction with the main WRPI dialog while planning is visible).

### 5.2 Controls

| Control | Description |
|---------|-------------|
| Base route selector | Read-only label showing the currently-selected route's Start → End. Updated when the user changes route selection in the main dialog. |
| **Time display** | `wxRadioButton` pair: "UTC" / "Local time". Default: UTC. When "Local time" selected, activates offset control. |
| — UTC offset | `wxSpinCtrl` showing signed offset in hours and minutes (format `UTC+H:MM` / `UTC−H:MM`); range −12:00 to +14:00, step 30 min. Active only when "Local time" selected. Persisted across sessions. |
| Departure window start | `wxDatePickerCtrl` + `wxTimePickerCtrl`. Shown and entered in the selected display timezone. Converted to UTC internally. Defaults to current date/time (in display timezone) rounded to next GRIB step. |
| Departure window end | Same; defaults to `sweep_start + 5 days`. |
| Step size | `wxChoice`: "1 h" / "2 h" / **"3 h" (default)** / "6 h" / "12 h" / "24 h" / "GRIB spacing (auto)". Finer steps produce more candidates and can surface departures between GRIB records that are genuinely better; all steps ≥ DeltaTime are fully valid. |
| Ranking mode | `wxChoice`: "Balanced" / "Fastest" / "Smoothest". Default: Balanced. |
| Balanced weight | `wxSlider` (0–10, displayed as 0.0–1.0). Visible only when mode = Balanced. |
| Comfort profile | `wxTextCtrl` (read-only path) + "Browse…" button + status label. |
| **Arrival window** | `wxCheckBox` "Constrain arrival time". When unchecked, all controls below are disabled. |
| Arrival mode | `wxRadioButton` pair: "Fixed time window (UTC)" / "Before sunset at destination". Default: Before sunset. |
| — Fixed: earliest | `wxTimePickerCtrl`. Shown and entered in display timezone. Default `06:00` (display). Visible only in Fixed mode. |
| — Fixed: latest | `wxTimePickerCtrl`. Shown and entered in display timezone. Default `20:00` (display). Visible only in Fixed mode. |
| — Sunset: margin | `wxSpinCtrlDouble` "Hours before sunset" (0.0–6.0, step 0.5). Default `0.0`. Visible only in Before sunset mode. |
| Results list | `wxListCtrl` (virtual, report mode); columns defined in §5.3. |
| Progress bar | `wxGauge`; visible during sweep, hidden otherwise. |
| Status text | One line; shows sweep state, warnings, error. |
| Run / Cancel | `wxButton`; "Run" starts WR.1; "Cancel" stops it; label toggles. |
| Apply | `wxButton`; copies selected candidate's `departure_utc` into the base configuration's `StartTime`; disabled when no candidate is selected or sweep is running. |
| Inspect | `wxButton`; opens the Candidate Detail view for the selected candidate (§5.5); disabled when no succeeded candidate is selected or sweep is running. |
| Close | `wxButton`; stops any running sweep, frees all sweep `RouteMapOverlay` objects, hides dialog. |

### 5.3 Results list columns

Column headers for time columns are dynamic: when UTC mode is active they read
`"Departure (UTC)"` and `"ETA (UTC)"`; when local time mode is active they read
`"Departure (UTC+H:MM)"` and `"ETA (UTC+H:MM)"` (or `"UTC−H:MM"` for negative offsets).
The `SweepCandidate` always stores UTC; conversion is applied at render time only.

| Column | Content | When failed |
|--------|---------|-------------|
| Rank | Integer or "—" | "—" |
| Departure | `YYYY-MM-DD HH:MM` in display timezone | Same |
| ETA | `YYYY-MM-DD HH:MM` in display timezone | "—" |
| Duration | `Xd HH:MM` | "—" |
| Avg TWS (kn) | One decimal | "—" |
| Max TWS (kn) | One decimal — worst wind encountered on route | "—" |
| Max Swell (m) | One decimal — peak significant wave height; "n/a" when GRIB has no swell | "—" |
| Comfort | Penalty formatted as "0.00" or "n/a" | "—" |
| Arrival | "✓" when `arrival_ok`; short `arrival_miss` text when `!arrival_ok`; "—" when filter disabled or candidate failed | "—" |
| Status | "OK" or routing failure reason | Short failure reason |

Row styling by tier:
- **Tier 1** (`succeeded && arrival_ok`): normal style.
- **Tier 2** (`succeeded && !arrival_ok`): italicised text to signal deprioritisation; not hidden.
- **Tier 3** (`!succeeded`): dimmed text.

The Arrival column is hidden when the arrival window filter is disabled (checkbox off).
Re-sort is applied immediately when ranking mode, weight, or arrival filter settings change,
without re-running the sweep.

### 5.4 Apply semantics

Clicking Apply:
- Reads `departure_utc` from the selected `SweepCandidate`.
- Calls `routemapoverlay->GetConfiguration()`, sets `StartTime = departure_utc`,
  clears `UseCurrentTime`, calls `routemapoverlay->SetConfiguration(configuration)`.
- Does **not** add a new route entry; modifies the existing base configuration in place.
- Does **not** automatically start a new routing run; the user initiates that from the
  main WRPI dialog as normal.

### 5.5 Candidate Detail View (Inspect)

Clicking **Inspect** on a succeeded candidate opens WRPI's existing per-route dialogs
on that candidate's retained `RouteMapOverlay`, without adding it to the main route list.

Two dialogs are opened (or raised if already open):

| Dialog | What it shows |
|--------|---------------|
| `StatisticsDialog` | Aggregated passage statistics: total distance, average and peak boat speed, total tacking/jibing count, time under power, average current, etc. Identical to the Statistics view for a normal WRPI route. |
| `PlotDialog` | Time-series plots of wind speed, boat speed, current, swell, and other parameters along the route. The user can pan and zoom to inspect any part of the passage. |

Both dialogs are pointed at the candidate's `RouteMapOverlay` directly. They are the
same classes used by the main WRPI UI (`WeatherRouting::m_StatisticsDialog`,
`WeatherRouting::m_PlotDialog`); no new dialog classes are required.

**Lifetime:** The `RouteMapOverlay` for each succeeded candidate is retained by
`DepartureSweepController` for the duration of the `DeparturePlanningDialog` session.
When the dialog closes (or a new sweep starts), all retained overlays are freed and the
Inspect dialogs are dismissed. A new sweep frees the previous sweep's overlays even if
the dialog remains open.

**Inspect is read-only.** The user cannot modify the candidate's route from Inspect.
To use the route, they press Apply (which plants the departure time) and then run the
route manually from the main WRPI dialog.

---

## 6. WR.4 — WindTrack Comfort Profile Loader

### 6.1 File format

WindTrack exports comfort profiles per `docs/COMFORT_PROFILE.md` schema v1 (reference:
`DEPARTURE_PLANNING.md` §7.5, frozen 2026-05-20). The profile is a JSON file with fields:
`schema_version` (int, must be 1), `model_kind` (string, must be `"threshold_bins"`),
`input_features` (array of strings), `parameters` (array of feature objects each with
`edges` and `penalties` arrays), `provenance`, `profile_id`, `valid_until`.

### 6.2 Validation

On load (`Browse…` or on dialog open if path is persisted), WRPI SHALL validate:

1. `schema_version == 1`.
2. `model_kind == "threshold_bins"`.
3. `parameters` length == `input_features` length.
4. For each feature: `len(penalties) == len(edges) + 1`.
5. `edges` strictly ascending.
6. All `penalties` in `[0, 1]`.

On validation failure: status label shows `"Invalid profile: <reason>"`; `comfort_penalty`
is treated as NaN for all candidates; GRIB-proxy ranking is used instead. No crash.

On validation success: status label shows `"Loaded: <profile_id>"`. If `valid_until` is
present and in the past, add `" (expired)"` to the label as advisory text.

### 6.3 Scoring a candidate route

After a candidate's `RouteMapOverlay` finishes successfully:

1. Walk the `PlotData` chain (the completed route's waypoints in order).
2. For each waypoint, extract:
   - `tws_kn` from `PlotData`'s wind data (knots).
   - `twa_deg = min(twa, 360 - twa)` normalised to `[0, 180]`.
   - `wvht_m` and `wvper_s` from swell data if present in GRIB and
     if `"wvht_m"` and `"wvper_s"` are in `input_features`; otherwise skip wave features.
3. For each waypoint, look up each feature's bin index using the feature's `edges` array
   (same `BinIndex` logic as WindTrack: `edges[i-1] <= x < edges[i]`; below first edge → bin 0;
   above last edge → last bin).
4. Look up `penalties[bin_index]` for each feature.
5. Average the per-feature penalties across all waypoints: `comfort_penalty = mean over waypoints of mean over features`.
6. Clamp to `[0, 1]`.

If the PlotData chain is empty (degenerate route), `comfort_penalty = NaN`.

### 6.4 Persistence

The comfort profile file path is persisted in wx config under:
`/PlugIns/WeatherRouting/DeparturePlanning/ComfortProfilePath`

The ranking mode, balanced weight, and display timezone are persisted under:
`/PlugIns/WeatherRouting/DeparturePlanning/RankMode`,
`/PlugIns/WeatherRouting/DeparturePlanning/BalancedWeight`, and
`/PlugIns/WeatherRouting/DeparturePlanning/DisplayTzOffsetMin`.

---

## 7. Code Touchpoints

| File | Change |
|------|--------|
| `src/WeatherRouting.h` | Add `void OpenDeparturePlanning();`; add `DeparturePlanningDialog* m_departurePlanningDialog = nullptr;` |
| `src/WeatherRouting.cpp` | Add button/toolbar entry; implement `OpenDeparturePlanning()` (create dialog if null, Show); destroy in destructor. |
| `src/weather_routing_pi.cpp` | Wire `GRIB_TIMELINE` availability to `DeparturePlanningDialog::OnGribTimelineAvailable()` if dialog is open. |
| `src/DepartureSweepController.h/cpp` | **New.** Owns sweep queue, throttle, thread management (WR.1+WR.2), and `RouteMapOverlay` lifetime for the Inspect action. Retains all succeeded overlays until `FreeOverlays()` is called (on dialog close or new sweep start). No wx dependency beyond `wxDateTime`. |
| `src/DeparturePlanningDialog.h/cpp` | **New.** wx dialog implementing WR.3. Calls `DepartureSweepController`; receives results via `wxCallAfter`. |
| `src/ComfortProfileLoader.h/cpp` | **New.** Load, validate, and score (WR.4). No wx in this class. Uses jsoncpp (already a WRPI dependency). |
| `src/SolarCalculator.h/cpp` | **New.** Standalone sunset computation (§3.6.2). No wx, no OpenCPN API. Unit-testable. |

No modifications to `RouteMap.h/cpp`, `IsoRoute.h/cpp`, `Position.h/cpp`, or any existing
routing algorithm. WR.1 uses the existing `RouteMapOverlay` API unchanged.

---

## 8. Invariants and Degraded Behaviour

| Invariant | Rule |
|-----------|------|
| `step_s >= DeltaTime` | A departure step finer than the isochrone propagation step is nonsensical. Enforce at input validation; show error if violated. Step size need not align with GRIB record spacing; `WeatherDataProvider` interpolates transparently. |
| Peak stats sampled from PlotData | `max_tws_kn` and `max_swell_m` are computed by a single pass over the `PlotData` chain immediately after the routing thread completes, in the same pass as `avg_tws_kn`. |
| Overlay lifetime | Succeeded `RouteMapOverlay` objects are freed on dialog close or new sweep start — whichever comes first. Failed-run overlays are freed immediately after result collection. |
| Inspect does not modify route list | The Inspect action never calls `WeatherRouting::AddConfiguration()` or any method that mutates `m_WeatherRoutes`. |
| `sweep_start < sweep_end` | Validate at Run time; show error if violated. |
| GRIB required | If GRIB timeline is unavailable at Run time, show error "No GRIB loaded" and abort. |
| No base route selected | If no route is selected in the main dialog, "Departure Planning…" button is disabled. |
| Failed routes shown, not hidden | Failed candidates appear at the bottom of the results list with a reason code. Hiding them would obscure useful information (e.g. all night departures fail due to constraints). |
| Profile missing / invalid | Fall back to GRIB-proxy ranking silently (status label indicates the fallback). No crash, no silent wrong scoring. |
| Wave features absent from GRIB | If profile requests `wvht_m`/`wvper_s` but GRIB has no swell data, skip wave features for scoring; note in status label. |
| Sweep window clamped | If user's window extends beyond GRIB valid range, clamp and warn; do not fail. |
| Apply only modifies StartTime | Apply does not change any other field of the base configuration and does not trigger a new routing run. |
| Re-ranking is instant | Changing sort mode, weight, or arrival filter settings re-sorts the in-memory list only; no re-running the engine. |
| Arrival filter: deprioritise, never discard | Tier 2 candidates (`succeeded && !arrival_ok`) are always shown in the results list. The user may still Apply them. |
| Sunset requires destination lat/lon | Mode B uses `RouteMapConfiguration.EndLat` / `EndLon`. If these are NaN or unset, Mode B is disabled in the UI (greyed out) and an explanatory label is shown. |
| Midnight sun: always OK | When `SolarCalculator::Sunset()` returns `wxInvalidDateTime` (midnight sun), `arrival_ok = true`. |
| Polar night: always missed | When `SolarCalculator` sets `polar_night = true`, `arrival_ok = false` with a specific miss message. |
| Filter is post-computation | `arrival_ok` is evaluated after routing completes, not during. Routing never skips a candidate based on the arrival filter. |
| Display timezone is render-only | `SweepCandidate.departure_utc` and `eta_utc` are always UTC `wxDateTime` values. Timezone conversion happens only in the list rendering path. No UTC value is ever stored in local time. |
| `display_tz_offset_min` range | −720 (UTC−12:00) to +840 (UTC+14:00). Values outside this range are clamped on load. |
| Changing timezone updates all time displays instantly | Switching UTC ↔ local (or changing the offset) refreshes all displayed times and column headers without re-running the sweep. Arrival window miss-reason strings are also regenerated. |
| Fixed window defaults are in display timezone | The defaults `06:00` / `20:00` are in the current display timezone. When the timezone is changed after setting the window, displayed times update; the underlying UTC filter boundary is recalculated to preserve the displayed intent. |

---

## 9. Testing

### 9.1 Unit tests (no OpenCPN runtime required)

| Test | What it covers |
|------|---------------|
| `test_departure_sweep_ranking` | `SweepCandidate` list with mix of success/fail; verify rank assignments and sort order for FASTEST, SMOOTHEST, BALANCED modes; verify failed candidates sort below succeeded. |
| `test_departure_sweep_ranking_balanced_weight` | w=0 → identical to FASTEST; w=1 → comfort-dominated; verify monotonicity. |
| `test_comfort_profile_loader_valid` | Well-formed 2-feature and 4-feature JSON profiles; verify load, validation pass, scoring of a mock PlotData chain. |
| `test_comfort_profile_loader_invalid` | Missing schema_version, wrong model_kind, mismatched penalties length, non-ascending edges, out-of-range penalty; verify each returns validation error and no score. |
| `test_comfort_profile_loader_expired` | `valid_until` in the past; verify load succeeds (profile still scores), advisory flag set. |
| `test_sweep_candidate_window_clamp` | Candidate list generation with `sweep_end` beyond a mock GRIB horizon; verify clamping and warning flag. |
| `test_arrival_filter_fixed_window` | ETA inside window → `arrival_ok=true`; ETA outside → `arrival_ok=false` with correct `arrival_miss`; wrap-midnight window (e.g. 22:00–04:00) handled correctly. |
| `test_arrival_filter_before_sunset` | Known ETA before computed sunset → `arrival_ok=true`; after → `arrival_ok=false`; margin applied correctly (sunset−2h boundary); disabled filter → all `arrival_ok=true`. |
| `test_arrival_filter_polar_cases` | Midnight-sun date at high latitude → `arrival_ok=true`; polar-night date → `arrival_ok=false` with polar-night message. |
| `test_solar_calculator_known_values` | Sunset at known lat/lon/date (e.g. 51.5°N 0°W 2026-06-21) vs NOAA reference to within ±5 minutes. At least three reference points covering northern/southern hemisphere and equinox/solstice. |
| `test_ranking_three_tier` | Mixed candidates (succeeded+ok, succeeded+missed, failed); verify Tier 1 above Tier 2 above Tier 3 for all sort modes; verify rank sequence is continuous across Tier 1+2. |
| `test_display_tz_formatting` | UTC candidate `departure_utc = 2026-06-15 14:00 UTC`; offset +9:30 → displayed as `"2026-06-15 23:30"`; offset −5:00 → `"2026-06-15 09:00"`; offset 0 → `"2026-06-15 14:00"`. |
| `test_display_tz_arrival_filter_fixed` | User enters window `06:00`–`20:00` at UTC+9:30 (= `20:30`–`10:30` UTC); verify stored UTC boundaries; verify ETA 22:00 UTC (= 07:30 local) is `arrival_ok`; ETA 14:00 UTC (= 23:30 local) is `!arrival_ok`. |
| `test_display_tz_miss_reason_label` | With offset UTC+3:00, miss-reason strings use `"UTC+3:00"` label, not raw `"UTC"`. |
| `test_peak_stats_extraction` | Mock `PlotData` chain with known per-waypoint TWS values (e.g. 8, 15, 22, 12 kn); verify `avg_tws_kn = 14.25`, `max_tws_kn = 22.0`; similar for swell values; NaN when chain is empty. |
| `test_peak_stats_no_swell` | `PlotData` chain with no swell data → `max_swell_m = NaN`; `avg_tws_kn` and `max_tws_kn` still populated correctly. |
| `test_step_size_candidates` | Sweep window 0 h – 12 h; step 3 h → 5 candidates (0, 3, 6, 9, 12); step 6 h → 3 candidates (0, 6, 12); step = DeltaTime-1 → validation error. |

### 9.2 Regression

All existing WRPI unit tests (`Polar_tests`, `IsoRoute_tests`, `PolygonRegion_tests`,
`Position_tests`, `RoutePoint_tests`, `Utilities_tests`) SHALL continue to pass.
No existing routing algorithm is modified.

---

## 10. Exit Criteria

Phase is complete when:

1. "Departure Planning…" opens `DeparturePlanningDialog` from the WRPI main dialog.
2. A departure sweep runs for a configured window, produces `SweepCandidate` results, and
   shows progress during computation.
3. Cancellation stops all running and pending threads promptly (within 5 s).
4. Results are ranked and displayed in the table with correct column values.
5. Failed candidates appear at the bottom with reason codes; no hiding.
6. Sort mode and balanced weight can be changed without re-running the sweep.
7. A WindTrack comfort profile file can be loaded; `comfort_penalty` column populates;
   validation errors produce a status message and fall back to GRIB proxy without crash.
8. `valid_until` expiry is indicated in the status label but does not prevent scoring.
9. Apply sets `StartTime` on the base configuration's `RouteMapOverlay` and does nothing else.
10. Arrival window filter (§3.6): both Fixed and Before-sunset modes correctly classify
    candidates; Tier 2 candidates are visible in the list with italicised text and a
    reason string; toggling the filter re-sorts without re-running.
11. Polar edge cases: midnight sun → `arrival_ok=true`; polar night → `arrival_ok=false`
    with correct message.
12. Local time display: switching to a non-zero UTC offset updates all displayed times,
    column headers, departure window pickers, arrival window time pickers, and miss-reason
    strings. No UTC value changes in the data model.
13. Results table shows Max TWS and Max Swell columns with correct values; Max Swell shows
    "n/a" when the loaded GRIB has no swell data.
14. Inspect opens `StatisticsDialog` and `PlotDialog` on the selected candidate's route;
    the main route list is unchanged. Closing the departure planning dialog dismisses
    the Inspect dialogs and frees the retained overlays.
15. Step size of 3 h on a 6-hour GRIB produces candidates at 0 h, 3 h, 6 h, 9 h, 12 h, …
    (confirmed in visual verification — all candidates compute successfully).
16. All unit tests in §9.1 pass.
17. All existing WRPI unit tests continue to pass (no regression).
18. Visual verification in OpenCPN: sweep produces plausible ranked results for a known
    start/end with GRIB loaded; Apply + manual route run from main dialog completes without error.

---

## 11. Revision History

| Version | Date | Notes |
|---------|------|-------|
| Draft-1 | 2026-05-21 | Initial contract. WR.1–WR.4 defined. References `DEPARTURE_PLANNING.md` for goals and phasing. |
| Draft-2 | 2026-05-21 | Added arrival window filter (§3.6): Fixed UTC time-of-day window and Before-sunset modes. `SweepCandidate` gains `arrival_ok`/`arrival_miss`. Three-tier ranking (§4.2). `SolarCalculator` utility (§3.6.2). Arrival column in results table. Polar edge cases. Six new unit tests. |
| Draft-3 | 2026-05-21 | Added local-time display option (§3.1, §3.6.1, §3.6.3, §5.2, §5.3): `display_tz_offset_min` parameter (−720 to +840 min); UTC/Local radio toggle + offset spinner in dialog; dynamic column headers; departure/ETA pickers and arrival filter time pickers all use display timezone; UTC stored internally throughout; miss-reason strings use display timezone label. Four new unit tests. |
| Draft-4 | 2026-05-21 | Step size default changed from GRIB spacing to 3 h; options expanded to 1 h/2 h/3 h/6 h/12 h/24 h/GRIB spacing; GRIB interpolation note added to §3.2. `SweepCandidate` gains `max_tws_kn` and `max_swell_m`. Max TWS and Max Swell columns added to results table. New §5.5 Candidate Detail View: Inspect button opens existing `StatisticsDialog` + `PlotDialog` on selected candidate's retained `RouteMapOverlay`; overlay freed on dialog close or new sweep. Three new unit tests. |
| **FINAL-1** | **2026-05-21** | **Frozen by custodian. Exit criteria renumbered (items 17–18). No content changes from Draft-4.** |

---

## 12. Approval Record

| Role | Status |
|------|--------|
| Author | Approved — 2026-05-21 |
| Custodian | Approved — 2026-05-21 (Patrick Gorman) |
