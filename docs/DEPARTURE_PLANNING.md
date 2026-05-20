# Departure planning — working framework

Design notes for a Weather Routing plugin feature: scan candidate departure times across available GRIB weather, reuse existing isochrone routing, and help the user pick a departure according to their priorities.

This document is a living framework; defaults and metrics can change as we implement and learn.

---

## 1. Problem

Sailors want to choose a **departure time** given:

- One or more GRIB sources loaded in OpenCPN (e.g. core GRIB plugin, PredictWind files when loaded the same way).
- The boat’s **polar** and routing constraints already modeled in WRPI.
- Sometimes a **delivery window** or other schedule pressure.
- Often **subjective** goals: comfort, avoiding night arrival at destination, etc.

“Best” is not universal; the feature must support **different goals** without hiding them behind a single score.

---

## 2. Fit inside WRPI

| Layer | Role |
|--------|------|
| **Inner** | Existing WRPI route computation: given `StartTime`, start/end, boat, GRIB — produce a route and ETA/duration. |
| **Outer** | New **orchestration**: step candidate departure times across the useful GRIB window, run (or schedule) inner solves, collect results, filter/rank. |

**Reuse, do not reimplement:** polars, GRIB access (`WeatherDataProvider`, GRIB plugin messages), `RouteMap` / `RouteMapOverlay`, `RouteMapConfiguration.StartTime`.

**Precedent:** **Configuration → Batch** (`GenerateBatch`) already varies `StartTime` over a span and generates multiple configurations. Departure planning extends that idea toward **GRIB-aligned bounds**, **ranking**, and **user-defined objectives**.

**GRIB scope:** WRPI sees whatever the GRIB plugin exposes (timeline, records). Supporting two unrelated GRIB stacks simultaneously is out of scope unless we explicitly design merging or “active dataset” selection later.

---

## 3. User model: constraints, preferences, presets

### 3.1 Hard constraints (filters)

A candidate either **passes** or **fails**. Examples:

- Arrival must fall in a calendar window (delivery).
- Wind/swell above a **hard** limit (if modeled as never acceptable).
- “No night landfall” when the user sets it as **hard** (ETA local time must lie in allowed daylight hours).

Failed candidates can be hidden or shown dimmed with reason codes.

### 3.2 Soft preferences (scoring)

Among passing candidates, **rank** by weighted or ordered criteria, for example:

- Shorter passage duration vs smoother conditions (“comfort”).
- Penalties for rough bins (once we define proxies: wind, waves, upwind fraction, etc.).

### 3.3 Presets

Named starting profiles so users are not forced to tune everything on day one, for example:

- **Delivery** — emphasize meeting arrival window and schedule risk.
- **Passage comfort** — emphasize comfort proxies and acceptable duration second.
- **Custom** — full control.

Presets are bundles of constraint/preference settings; they are not separate engines.

### 3.4 Custom settings

**Custom** exposes:

- **Per concern:** mode **Hard constraint** vs **Strong preference** (default choice is TBD and may change in a later release).

  - **Hard:** filter candidates that violate the rule.
  - **Strong preference:** penalize in scoring but still show viable options when no candidate fully satisfies the preference.

- **Dial-in:** weights/thresholds for comfort vs time, local ETA bands for night avoidance when in preference mode, etc.

Same physics for everyone; only the **policy** (filters + scores) changes.

---

## 4. Open items (to refine during implementation)

- **Default for night landfall:** hard vs strong preference — pick one for v1; Custom always overrides.
- **Comfort metrics:** start minimal (e.g. duration + caps on wind/swell from route/GRIB along track); enrich from WRPI statistics/plots if available.
- **Sweep grid:** step size vs GRIB timestep; alignment with `GRIB_TIMELINE` extent.
- **UX:** new dialog vs evolution of Batch; table/chart of candidates; “apply this departure to the active route configuration.”

---

## 5. Phased delivery (suggested)

| Phase | Focus |
|--------|--------|
| **A** | Spec the first objective: constraints list + one ranking function for a single preset (e.g. comfort + duration). |
| **B** | Implement sweep over `StartTime` within GRIB-valid window; collect end time, duration, success/fail per run. |
| **C** | Filtering + ranking UI; presets + Custom (hard vs preference per concern). |
| **D** | Polish: empty results messaging, performance (queue/limit runs), optional arrival-window logic from WindTrack if still separate. |

---

## 6. Code touchpoints (for developers)

- Orchestration: `WeatherRouting` — patterns near `GenerateBatch()`, `Start()`, route list updates.
- GRIB timeline: `weather_routing_pi.cpp` (`GRIB_TIMELINE`, `GRIB_TIMELINE_RECORD`), configuration dialog timeline fields.
- Results: `RouteMapOverlay::EndTime()`, `WeatherRoute` display fields (start/end/duration).
- New UI: follow existing dialog patterns (e.g. `ConfigurationBatchDialog`); update `WeatherRoutingUI` / FB if adding controls.

---

## 7. Optional: WindTrack-learned comfort

WindTrack records **IMU `.motion`** time series and optional **JSON log events** (timestamped comfort notes). Correlating those yields a **personal** notion of discomfort that generic GRIB-only proxies cannot capture. WRPI can treat this as an **optional** scoring layer on top of departure sweep ranking.

### 7.1 Design principles

- **WRPI does not require WindTrack** for core departure planning. If no profile is loaded or WindTrack is absent, behavior falls back to built-in comfort presets (GRIB-derived proxies only).
- **Raw logs stay in WindTrack.** WRPI should consume a **small, versioned comfort profile** produced offline (or by WindTrack export), not parse `.motion` binaries directly in the first iteration.
- **Honest uncertainty:** sparse JSON labels imply wide confidence intervals; the UI should not overstate “likelihood” until the model and data volume justify it.

### 7.2 Integration shape (target)

| Piece | Role |
|--------|------|
| **WindTrack** | Ingest `.motion` + `.json`, correlate, fit or calibrate a compact model, write **comfort profile** file(s). |
| **WRPI** | If enabled and profile present, map each candidate route’s along-track conditions (from GRIB + route geometry + polar context) to inputs the profile expects, compute an extra **discomfort penalty or likelihood**, and fold it into Custom ranking. |
| **Detection** | Optional WindTrack presence via plugin API / message convention / documented install path — exact mechanism TBD; profile file path may be enough for v1 (user points WRPI at an exported file). |

### 7.3 Comfort profile export — requirements for WindTrack (draft)

Use this as the checklist when writing the WindTrack-side spec; the **canonical schema** may later live in the WindTrack repo or a shared snippet once frozen.

**Metadata (required)**

- **Schema version** — integer or semver so WRPI can reject unknown formats safely.
- **Profile id** — stable string (e.g. boat + sensor calibration id).
- **Created / valid range** — UTC timestamps; optional expiry if sensors or crew sensitivity drift.
- **Provenance** — counts and dates of source `.motion` sessions and `.json` events used to build the profile.

**Model declaration**

- **Model kind** — e.g. `threshold_bins`, `piecewise_linear`, `lookup_table`, `disabled`; extensible enum.
- **Input features** — ordered list of **names** the evaluator expects (see below). WRPI maps route/GRIB outputs to these; unknown features disable the layer or trigger fallback.
- **Parameters** — model-specific payload (coefficients, bin edges, table paths). Keep machine-readable JSON or a single sidecar binary with a JSON header.

**Feature vocabulary (negotiate with WRPI)**

WindTrack should document which signals it can relate to discomfort; WRPI will implement mapping where possible. Typical candidates (final list TBD):

- Along-route **TWS**, **TWA** (or AWA), **significant wave height**, **wave period**, **swell direction delta** vs heading.
- Optional aggregates over a leg: fraction of time above thresholds, max values, etc.

Features **not** available from WRPI+GRIB alone may require WindTrack to embed **derived proxies** from historical correlation only (document limitation).

**Semantics**

- **Output type** — scalar penalty (higher = worse), or probability in \([0,1]\), plus units.
- **Calibration notes** — e.g. “sparse labels,” minimum recommended passage samples.

**Privacy / portability**

- Profile should contain **no raw motion samples** unless explicitly opted in; ship aggregates/parameters only.

### 7.4 WRPI behavior summary

| Condition | Behavior |
|-----------|----------|
| No profile / integration off | Ranking uses generic comfort preferences only. |
| Profile loaded, model `disabled` | Same as above; log optional notice. |
| Profile loaded, model active | Add WindTrack term to score; surface in UI as “personal comfort (WindTrack).” |

### 7.5 Tracking

- **WindTrack comfort profile specification — FROZEN 2026-05-20.**
  Canonical schema: `docs/COMFORT_PROFILE.md` in the WindTrack repo.
  Schema v1: `threshold_bins`, additive penalties, four-feature vocabulary
  (`tws_kn`, `twa_deg`, `wvht_m`, `wvper_s`). Wind-only `[tws_kn, twa_deg]` and
  wave-augmented `[tws_kn, twa_deg, wvht_m, wvper_s]` profiles both supported;
  `input_features` list is self-describing.

---

## 8. Revision history

| Date | Note |
|------|------|
| 2026-05-01 | Initial framework from design discussion. |
| 2026-05-01 | Added §7 Optional WindTrack-learned comfort and draft export requirements for WindTrack. |
| 2026-05-20 | §7.5 updated — WindTrack COMFORT_PROFILE.md frozen at schema v1. |
