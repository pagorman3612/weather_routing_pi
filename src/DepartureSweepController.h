/*
 * DepartureSweepController.h — WR.1 + WR.2 + WR.3 Departure Sweep
 *
 * Static engine helpers are fully unit-testable without RouteMapOverlay.
 * Async orchestration (StartSweep / Poll / FreeOverlays) is coupled to
 * RouteMapOverlay and runs on the UI thread via wxTimer.
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

#include "RouteMap.h"        // PlotData, RouteMapConfiguration
#include "ComfortProfileLoader.h"
class RouteMapOverlay;       // full type in DepartureSweepController.cpp

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

    static CandidateListResult BuildCandidateList(
        const wxDateTime& sweep_start,
        const wxDateTime& sweep_end,
        const wxDateTime& grib_start,
        const wxDateTime& grib_end,
        int step_s,
        int delta_time_s);

    static void ExtractPlotStats(
        const std::list<PlotData>& data,
        double& avg_tws_kn,
        double& max_tws_kn,
        double& max_swell_m,
        double& upwind_fraction);

    static bool EvaluateArrivalWindow(
        const wxDateTime& eta_utc,
        const ArrivalWindowParams& params,
        wxString& miss_reason);

    static wxString FormatDisplayTime(
        const wxDateTime& utc,
        int tz_offset_min,
        const wxString& fmt = "%Y-%m-%d %H:%M");

    static wxString FormatTzLabel(int tz_offset_min);

    static void RankCandidates(
        std::vector<SweepCandidate>& candidates,
        const RankParams& params);

    // Build WaypointSample vector from PlotData for comfort scoring.
    static std::vector<WaypointSample> ExtractWaypointSamples(
        const std::list<PlotData>& data);

    // ------------------------------------------------------------------
    // Async sweep orchestration (WR.3 — UI-thread, no OpenCPN-free tests)
    // ------------------------------------------------------------------

    struct SweepConfig {
        RouteMapConfiguration   base_config;
        std::vector<wxDateTime> departures;
        int                     max_concurrent    = 4;
        ArrivalWindowParams     arrival_params;
        RankParams              rank_params;
        ComfortProfileLoader*   profile           = nullptr; // not owned
    };

    DepartureSweepController() = default;
    ~DepartureSweepController() { FreeOverlays(); }

    // Start a new sweep. Frees any previous overlays first.
    void StartSweep(const SweepConfig& cfg);

    // Request cancellation. Already-running overlays are stopped; partial
    // results are ranked and displayed.
    void StopSweep();

    bool IsRunning()   const { return m_running; }
    bool IsCancelled() const { return m_cancelled; }
    int  CompletedCount() const { return m_completed_count; }
    int  TotalCount()     const { return (int)m_candidates.size(); }

    // Call from UI timer (~250 ms). Checks running overlays, handles GRIB
    // requests, collects results, dispatches pending work.
    // Returns number of newly-completed candidates (0 when idle).
    int Poll();

    const std::vector<SweepCandidate>& GetCandidates() const { return m_candidates; }

    void RerankCandidates(const RankParams& params) {
        RankCandidates(m_candidates, params);
    }

    void ReapplyArrivalFilter(const ArrivalWindowParams& params,
                              const RankParams& rank_params);

    // Free all retained RouteMapOverlay objects (succeeded candidates).
    // Called on dialog close or before a new sweep.
    void FreeOverlays();

    // Retrieve overlay for candidate index (for Inspect). May be nullptr.
    RouteMapOverlay* GetOverlay(size_t index) const;

private:
    void DispatchNext();
    void CollectCandidate(RouteMapOverlay* ov, size_t idx);

    bool       m_running         = false;
    bool       m_cancelled       = false;
    int        m_completed_count = 0;
    size_t     m_next_dispatch   = 0;

    SweepConfig m_config;

    struct RunEntry {
        RouteMapOverlay* overlay;
        size_t           idx;
    };
    std::list<RunEntry>           m_running_list;
    std::vector<SweepCandidate>   m_candidates;
    std::vector<RouteMapOverlay*> m_overlays; // retained for Inspect; nullptr = failed
};
