/*
 * DepartureSweepController.h — WR.1 + WR.2 Departure Sweep
 *
 * Static engine helpers are fully unit-testable without RouteMapOverlay.
 * The async orchestration class skeleton is here; full implementation
 * wires up in WR.3 (DeparturePlanningDialog).
 *
 * Contract: docs/DEPARTURE_SWEEP_CONTRACT_FINAL-1.md §3, §4.
 */
#pragma once

#include <wx/wx.h>      // must precede ODAPI.h (via RouteMap.h)

#include <cmath>
#include <functional>
#include <limits>
#include <list>
#include <vector>

#include "RouteMap.h"   // PlotData, RouteMapConfiguration
class RouteMapOverlay;  // forward-declaration only — full type in WR.3

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

struct SweepCandidate {
    wxDateTime  departure_utc;
    wxDateTime  eta_utc;
    wxTimeSpan  duration;
    bool        succeeded       = false;
    wxString    failure_reason;
    double      avg_tws_kn      = std::numeric_limits<double>::quiet_NaN();
    double      max_tws_kn      = std::numeric_limits<double>::quiet_NaN();
    double      max_swell_m     = std::numeric_limits<double>::quiet_NaN();
    double      upwind_fraction = std::numeric_limits<double>::quiet_NaN();
    double      comfort_penalty = std::numeric_limits<double>::quiet_NaN();
    bool        arrival_ok      = true;
    wxString    arrival_miss;
    int         rank            = 0;    // 1-based among succeeded; 0 = failed
};

enum class ArrivalFilterMode { FIXED_WINDOW, BEFORE_SUNSET };

struct ArrivalWindowParams {
    bool              enabled                 = false;
    ArrivalFilterMode mode                    = ArrivalFilterMode::BEFORE_SUNSET;
    int               arrival_earliest_utc    = 360;   // minutes from midnight UTC (Mode A)
    int               arrival_latest_utc      = 1200;  // minutes from midnight UTC (Mode A)
    double            sunset_margin_h         = 0.0;   // hours before sunset (Mode B)
    int               display_tz_offset_min   = 0;     // for miss-reason formatting
    double            end_lat                 = std::numeric_limits<double>::quiet_NaN();
    double            end_lon                 = std::numeric_limits<double>::quiet_NaN();
};

enum class SortMode { FASTEST, SMOOTHEST, BALANCED };

struct RankParams {
    SortMode mode            = SortMode::BALANCED;
    double   balanced_weight = 0.5;   // w in [0,1]
};

// Results from BuildCandidateList
struct CandidateListResult {
    std::vector<wxDateTime> departures;
    bool     clamped_start = false;
    bool     clamped_end   = false;
    wxString warning;
};

// ---------------------------------------------------------------------------
// DepartureSweepController
// ---------------------------------------------------------------------------

class DepartureSweepController {
public:
    // ------------------------------------------------------------------
    // Testable static helpers (no RouteMapOverlay, no threads)
    // ------------------------------------------------------------------

    // Build ordered list of candidate departure times within [sweep_start, sweep_end].
    // Clamps to [grib_start, grib_end] and populates result.clamped_* flags.
    // Returns empty departures list with warning if step_s < delta_time_s or
    // window collapses after clamping.
    static CandidateListResult BuildCandidateList(
        const wxDateTime& sweep_start,
        const wxDateTime& sweep_end,
        const wxDateTime& grib_start,
        const wxDateTime& grib_end,
        int step_s,
        int delta_time_s);

    // Single-pass extraction of avg/max TWS and peak swell from PlotData chain.
    // max_swell_m = NaN when all WVHT values are 0 (no swell data in GRIB).
    // avg_tws_kn / max_tws_kn / upwind_fraction = NaN when chain is empty.
    static void ExtractPlotStats(
        const std::list<PlotData>& data,
        double& avg_tws_kn,
        double& max_tws_kn,
        double& max_swell_m,
        double& upwind_fraction);

    // Evaluate whether eta_utc satisfies the arrival window.
    // Sets miss_reason when returning false.
    // When params.enabled == false always returns true.
    static bool EvaluateArrivalWindow(
        const wxDateTime& eta_utc,
        const ArrivalWindowParams& params,
        wxString& miss_reason);

    // Format a UTC wxDateTime for display using the given timezone offset (minutes).
    static wxString FormatDisplayTime(
        const wxDateTime& utc,
        int tz_offset_min,
        const wxString& fmt = "%Y-%m-%d %H:%M");

    // Format a timezone offset as "UTC", "UTC+H:MM", or "UTC-H:MM".
    static wxString FormatTzLabel(int tz_offset_min);

    // Rank candidates in-place according to three-tier rules (§4.2).
    // Re-entrant — call after any filter or mode change.
    static void RankCandidates(
        std::vector<SweepCandidate>& candidates,
        const RankParams& params);

    // ------------------------------------------------------------------
    // Async sweep orchestration — skeleton; full impl in WR.3
    // ------------------------------------------------------------------

    using CompletionCallback =
        std::function<void(const std::vector<SweepCandidate>&)>;

    DepartureSweepController() = default;

    bool IsRunning() const { return m_running; }

    const std::vector<SweepCandidate>& GetCandidates() const { return m_candidates; }

    void RerankCandidates(const RankParams& params) {
        RankCandidates(m_candidates, params);
    }

private:
    bool                        m_running    = false;
    std::vector<SweepCandidate> m_candidates;
};
