/*
 * DepartureSweepController.cpp — WR.1 + WR.2 + WR.3 Departure Sweep
 *
 * Contract: docs/DEPARTURE_SWEEP_CONTRACT_FINAL-1.md §3, §4.
 */
#include <wx/wx.h>
#include "DepartureSweepController.h"
#ifndef UNIT_TESTS
#include "RouteMapOverlay.h"
#endif
#include "SolarCalculator.h"

#include <algorithm>
#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

static const double   kNaN    = std::numeric_limits<double>::quiet_NaN();
static const wxString kEmDash(wchar_t(0x2014)); // — avoids UTF-8/ANSI mojibake
static const wxString kEnDash(wchar_t(0x2013)); // –

// Absolute angular difference between two compass headings, result in [0, 180].
double HeadingDiff(double a, double b) {
    double d = std::fabs(a - b);
    while (d >= 360.0) d -= 360.0;
    return d > 180.0 ? 360.0 - d : d;
}

// UTC time-of-day in minutes from midnight.
int EtaTod(const wxDateTime& eta_utc) {
    return eta_utc.GetHour(wxDateTime::UTC) * 60 + eta_utc.GetMinute(wxDateTime::UTC);
}

// Format HH:MM from minutes-from-midnight, applying tz_offset_min.
wxString FmtHHMM(int minutes_utc, int tz_offset_min) {
    int total = ((minutes_utc + tz_offset_min) % 1440 + 1440) % 1440;
    return wxString::Format("%02d:%02d", total / 60, total % 60);
}

} // namespace

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

CandidateListResult DepartureSweepController::BuildCandidateList(
    const wxDateTime& sweep_start,
    const wxDateTime& sweep_end,
    const wxDateTime& grib_start,
    const wxDateTime& grib_end,
    int step_s,
    int delta_time_s)
{
    CandidateListResult result;

    if (step_s < delta_time_s) {
        result.warning = wxString::Format(
            "Step size %d s is less than DeltaTime %d s — aborting", step_s, delta_time_s);
        return result;
    }

    // Clamp to GRIB valid range.
    wxDateTime clamped_start = sweep_start;
    wxDateTime clamped_end   = sweep_end;

    if (clamped_start < grib_start) {
        clamped_start = grib_start;
        result.clamped_start = true;
    }
    if (clamped_end > grib_end) {
        clamped_end = grib_end;
        result.clamped_end = true;
    }

    if (clamped_start > clamped_end) {
        result.warning = "Sweep window collapses to zero after GRIB clamping — aborting";
        return result;
    }

    // Build departure list.
    wxDateTime t = clamped_start;
    while (t <= clamped_end) {
        result.departures.push_back(t);
        t += wxTimeSpan::Seconds(step_s);
    }
    return result;
}

void DepartureSweepController::ExtractPlotStats(
    const std::list<PlotData>& data,
    double& avg_tws_kn,
    double& max_tws_kn,
    double& max_swell_m,
    double& upwind_fraction)
{
    avg_tws_kn      = kNaN;
    max_tws_kn      = kNaN;
    max_swell_m     = kNaN;
    upwind_fraction = kNaN;

    if (data.empty()) return;

    double sum_tws    = 0.0;
    double peak_tws   = 0.0;
    double peak_swell = 0.0;
    bool   has_swell  = false;
    int    n_upwind   = 0;
    int    n          = 0;

    for (const PlotData& p : data) {
        double tws = p.twsOverGround;
        sum_tws += tws;
        if (tws > peak_tws) peak_tws = tws;

        if (p.WVHT > 0.0) {
            has_swell = true;
            if (p.WVHT > peak_swell) peak_swell = p.WVHT;
        }

        // TWA = angular difference between TWD (from) and COG.
        // Upwind when TWA < 60°.
        double twa = HeadingDiff(p.twdOverGround, p.cog);
        if (twa < 60.0) ++n_upwind;

        ++n;
    }

    avg_tws_kn      = sum_tws / n;
    max_tws_kn      = peak_tws;
    max_swell_m     = has_swell ? peak_swell : kNaN;
    upwind_fraction = static_cast<double>(n_upwind) / n;
}

bool DepartureSweepController::EvaluateArrivalWindow(
    const wxDateTime& eta_utc,
    const ArrivalWindowParams& params,
    wxString& miss_reason)
{
    miss_reason = wxEmptyString;

    if (!params.enabled) return true;
    if (!eta_utc.IsValid()) return false;

    const wxString tz_label = FormatTzLabel(params.display_tz_offset_min);

    if (params.mode == ArrivalFilterMode::FIXED_WINDOW) {
        int eta_tod = EtaTod(eta_utc);
        int earliest = params.arrival_earliest_utc;
        int latest   = params.arrival_latest_utc;

        bool ok;
        if (earliest <= latest) {
            ok = (eta_tod >= earliest && eta_tod <= latest);
        } else {
            // Wrap-midnight window.
            ok = (eta_tod >= earliest || eta_tod <= latest);
        }

        if (!ok) {
            int eta_disp_min = ((eta_tod + params.display_tz_offset_min) % 1440 + 1440) % 1440;
            wxString eta_disp = wxString::Format("%02d:%02d",
                eta_disp_min / 60, eta_disp_min % 60);
            wxString win_early = FmtHHMM(earliest, params.display_tz_offset_min);
            wxString win_late  = FmtHHMM(latest,   params.display_tz_offset_min);
            miss_reason = wxString::Format("Arrives %s %s %s outside window %s%s%s %s",
                eta_disp, tz_label, kEmDash, win_early, kEnDash, win_late, tz_label);
        }
        return ok;
    }

    // Mode B — Before sunset at destination.
    if (std::isnan(params.end_lat) || std::isnan(params.end_lon)) {
        return true; // no destination coords — can't evaluate
    }

    bool polar_night = false;
    wxDateTime sunset = SolarCalculator::Sunset(
        eta_utc.GetYear(wxDateTime::UTC),
        static_cast<int>(eta_utc.GetMonth(wxDateTime::UTC)) + 1,
        eta_utc.GetDay(wxDateTime::UTC),
        params.end_lat, params.end_lon, &polar_night);

    if (polar_night) {
        miss_reason = "Polar night at destination on ETA date";
        return false;
    }
    if (!sunset.IsValid()) {
        // Midnight sun — always ok.
        return true;
    }

    // Apply margin.
    wxDateTime deadline = sunset;
    if (params.sunset_margin_h > 0.0) {
        long margin_s = static_cast<long>(params.sunset_margin_h * 3600.0);
        deadline -= wxTimeSpan::Seconds(margin_s);
    }

    int eta_tod      = EtaTod(eta_utc);
    int deadline_tod = deadline.GetHour(wxDateTime::UTC) * 60 + deadline.GetMinute(wxDateTime::UTC);

    bool ok = (eta_tod <= deadline_tod);
    if (!ok) {
        wxString eta_disp = FormatDisplayTime(eta_utc, params.display_tz_offset_min, "%H:%M");
        wxString set_disp = FormatDisplayTime(deadline, params.display_tz_offset_min, "%H:%M");
        wxString margin_str;
        if (params.sunset_margin_h > 0.0)
            margin_str = wxString::Format(" (-%s h buffer)",
                wxString::Format("%.4g", params.sunset_margin_h));
        miss_reason = wxString::Format("Arrives %s %s %s sunset%s %s %s",
            eta_disp, tz_label, kEmDash, margin_str, set_disp, tz_label);
    }
    return ok;
}

wxString DepartureSweepController::FormatDisplayTime(
    const wxDateTime& utc,
    int tz_offset_min,
    const wxString& fmt)
{
    if (!utc.IsValid()) return wxString(wchar_t(0x2014));
    // Add display offset, then extract via UTC accessors to avoid wxDateTime::Format()
    // bleeding in the machine's local timezone on non-UTC systems.
    wxDateTime shifted = utc + wxTimeSpan::Minutes(tz_offset_min);
    int h  = shifted.GetHour(wxDateTime::UTC);
    int mn = shifted.GetMinute(wxDateTime::UTC);
    if (fmt == "%H:%M")
        return wxString::Format("%02d:%02d", h, mn);
    // Default: full date+time (covers the "%Y-%m-%d %H:%M" default parameter).
    return wxString::Format("%04d-%02d-%02d %02d:%02d",
        shifted.GetYear(wxDateTime::UTC),
        (int)shifted.GetMonth(wxDateTime::UTC) + 1,
        shifted.GetDay(wxDateTime::UTC),
        h, mn);
}

wxString DepartureSweepController::FormatTzLabel(int tz_offset_min) {
    if (tz_offset_min == 0) return "UTC";
    int abs_min = std::abs(tz_offset_min);
    int h = abs_min / 60;
    int m = abs_min % 60;
    char sign = (tz_offset_min > 0) ? '+' : '-';
    return wxString::Format("UTC%c%d:%02d", sign, h, m);
}

void DepartureSweepController::RankCandidates(
    std::vector<SweepCandidate>& candidates,
    const RankParams& params)
{
    auto sort_key = [&](const SweepCandidate& c) -> double {
        if (!c.succeeded) return 0.0;
        double duration_h = c.duration.GetSeconds().ToDouble() / 3600.0;
        double comfort = std::isnan(c.comfort_penalty)
            ? (std::isnan(c.avg_tws_kn) ? 0.0 : c.avg_tws_kn / 30.0)
            : c.comfort_penalty;
        switch (params.mode) {
            case SortMode::FASTEST:   return duration_h;
            case SortMode::SMOOTHEST: return comfort;
            case SortMode::BALANCED:
                return duration_h * (1.0 + params.balanced_weight * comfort);
        }
        return duration_h;
    };

    // Partition into tiers.
    std::vector<SweepCandidate*> tier1, tier2, tier3;
    for (auto& c : candidates) {
        if (!c.succeeded)      tier3.push_back(&c);
        else if (c.arrival_ok) tier1.push_back(&c);
        else                   tier2.push_back(&c);
    }

    auto cmp = [&](const SweepCandidate* a, const SweepCandidate* b) {
        return sort_key(*a) < sort_key(*b);
    };
    std::sort(tier1.begin(), tier1.end(), cmp);
    std::sort(tier2.begin(), tier2.end(), cmp);
    std::sort(tier3.begin(), tier3.end(),
        [](const SweepCandidate* a, const SweepCandidate* b) {
            return a->departure_utc.IsEarlierThan(b->departure_utc);
        });

    // Assign ranks: 1-based across Tier 1 + Tier 2.
    int rank = 1;
    for (auto* c : tier1) c->rank = rank++;
    for (auto* c : tier2) c->rank = rank++;
    for (auto* c : tier3) c->rank = 0;

    // Reconstruct vector in tier order.
    std::vector<SweepCandidate> sorted;
    sorted.reserve(candidates.size());
    for (auto* c : tier1) sorted.push_back(*c);
    for (auto* c : tier2) sorted.push_back(*c);
    for (auto* c : tier3) sorted.push_back(*c);
    candidates = std::move(sorted);
}

std::vector<WaypointSample> DepartureSweepController::ExtractWaypointSamples(
    const std::list<PlotData>& data)
{
    std::vector<WaypointSample> out;
    out.reserve(data.size());
    for (const PlotData& p : data) {
        WaypointSample s;
        s.tws_kn  = p.twsOverGround;
        s.twa_deg = HeadingDiff(p.twdOverGround, p.cog);
        s.wvht_m  = p.WVHT;
        s.wvper_s = p.WVPER;
        out.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Async orchestration (WR.3)
// ---------------------------------------------------------------------------

#ifndef UNIT_TESTS

void DepartureSweepController::StartSweep(const SweepConfig& cfg,
                                           RouteMapOverlay** grib_slot) {
    FreeOverlays();

    m_config          = cfg;
    m_grib_slot       = grib_slot;
    m_running         = true;
    m_cancelled       = false;
    m_completed_count = 0;
    m_next_dispatch   = 0;

    const size_t n = cfg.departures.size();
    m_candidates.assign(n, SweepCandidate{});
    for (size_t i = 0; i < n; ++i) {
        m_candidates[i].departure_utc = cfg.departures[i];
        m_candidates[i].original_idx  = i;
    }
    // Dispatch first batch.
    int slots = m_config.max_concurrent;
    while (slots-- > 0 && m_next_dispatch < n)
        DispatchNext();
}

void DepartureSweepController::StopSweep() {
    m_cancelled = true;
    for (auto& e : m_running_list)
        e.overlay->Stop();
    // Poll() will collect completions and set m_running = false.
}

int DepartureSweepController::Poll(RouteMapOverlay** grib_slot) {
    if (!m_running) return 0;

    m_grib_slot = grib_slot; // keep current for DispatchNext calls this tick

    int newly_done = 0;

    for (auto it = m_running_list.begin(); it != m_running_list.end(); ) {
        RouteMapOverlay* ov = it->overlay;
        size_t idx           = it->idx;

        // Handle mid-computation GRIB requests.
        // Must set grib_slot before calling RequestGrib() — the GRIB plugin
        // responds synchronously via SetPluginMessage and reads that pointer
        // to know which overlay receives the data.
        if (ov->NeedsGrib() && !ov->Finished()) {
            if (grib_slot) *grib_slot = ov;
            ov->RequestGrib(ov->NewTime());
            if (grib_slot) *grib_slot = nullptr;
        }

        if (!ov->Running()) {
            ov->DeleteThread();
            CollectCandidate(ov, idx);
            ++m_completed_count;
            ++newly_done;
            it = m_running_list.erase(it);

            if (!m_cancelled)
                DispatchNext();
        } else {
            ++it;
        }
    }

    if (m_running_list.empty() &&
        (m_next_dispatch >= m_candidates.size() || m_cancelled)) {
        m_running = false;
        RankCandidates(m_candidates, m_config.rank_params);
    }

    return newly_done;
}

void DepartureSweepController::DispatchNext() {
    if (m_next_dispatch >= m_config.departures.size()) return;
    size_t idx = m_next_dispatch++;

    RouteMapConfiguration cfg = m_config.base_config;
    cfg.StartTime      = m_config.departures[idx];
    cfg.UseCurrentTime = false;

    auto* ov = new RouteMapOverlay;
    ov->SetConfiguration(cfg);
    ov->Reset(); // initializes m_NewTime=StartTime and m_bNeedsGrib (was missing)

    // Deliver initial GRIB before starting the thread — mirrors WeatherRouting::Start().
    // Without this the routing thread runs step 2 before GRIB arrives and
    // immediately fails with "Isochrone exceeds GRIB data range".
    if (m_grib_slot && cfg.UseGrib) {
        *m_grib_slot = ov;
        ov->RequestGrib(ov->NewTime());
        *m_grib_slot = nullptr;
    }

    wxString err;
    if (ov->Start(err)) {
        m_running_list.push_back({ov, idx});
    } else {
        SweepCandidate& c = m_candidates[idx];
        c.succeeded     = false;
        c.failure_reason = err.empty() ? wxString("Failed to start") : err;
        delete ov;
        ++m_completed_count;
    }
}

void DepartureSweepController::CollectCandidate(RouteMapOverlay* ov, size_t idx) {
    SweepCandidate& c = m_candidates[idx];
    wxString err         = ov->GetError();
    wxString forecasterr = ov->GetWeatherForecastError();

    if (err.empty() && ov->EndTime().IsValid()) {
        c.succeeded = true;
        c.eta_utc   = ov->EndTime();
        c.duration  = c.eta_utc - c.departure_utc;

        const std::list<PlotData>& pd = ov->GetPlotData();
        ExtractPlotStats(pd, c.avg_tws_kn, c.max_tws_kn, c.max_swell_m, c.upwind_fraction);

        if (m_config.profile && m_config.profile->IsLoaded()) {
            auto samples = ExtractWaypointSamples(pd);
            c.comfort_penalty = m_config.profile->Score(samples);
        }

        wxString miss;
        c.arrival_ok   = EvaluateArrivalWindow(c.eta_utc, m_config.arrival_params, miss);
        c.arrival_miss = miss;

        delete ov; // stats extracted; free immediately to conserve address space
    } else {
        c.succeeded = false;
        if (!err.empty())
            c.failure_reason = err;
        else if (!forecasterr.empty())
            c.failure_reason = forecasterr;
        else
            c.failure_reason = "Did not reach destination";
        delete ov;
    }
}

void DepartureSweepController::ReapplyArrivalFilter(
    const ArrivalWindowParams& params,
    const RankParams& rank_params)
{
    for (SweepCandidate& c : m_candidates) {
        if (!c.succeeded) continue;
        wxString miss;
        c.arrival_ok   = EvaluateArrivalWindow(c.eta_utc, params, miss);
        c.arrival_miss = miss;
    }
    RankCandidates(m_candidates, rank_params);
}

void DepartureSweepController::FreeOverlays() {
    // Stop and clean up any still-running sweep overlays.
    for (auto& e : m_running_list) {
        e.overlay->Stop();
        e.overlay->DeleteThread();
        delete e.overlay;
    }
    m_running_list.clear();

    // Overlays for succeeded candidates were already freed immediately after
    // stats extraction in CollectCandidate — nothing to free here.

    FreeInspectOverlay();

    m_candidates.clear();
    m_running         = false;
    m_cancelled       = false;
    m_completed_count = 0;
    m_next_dispatch   = 0;
}

// ---------------------------------------------------------------------------
// Inspect recompute
// ---------------------------------------------------------------------------

void DepartureSweepController::StartInspect(const wxDateTime& departure_utc,
                                             RouteMapOverlay** grib_slot) {
    FreeInspectOverlay();

    m_inspect_done   = false;
    m_inspect_failed = false;

    RouteMapConfiguration cfg = m_config.base_config;
    cfg.StartTime      = departure_utc;
    cfg.UseCurrentTime = false;

    auto* ov = new RouteMapOverlay;
    ov->SetConfiguration(cfg);
    ov->Reset();

    if (grib_slot && cfg.UseGrib) {
        *grib_slot = ov;
        ov->RequestGrib(ov->NewTime());
        *grib_slot = nullptr;
    }

    wxString err;
    if (ov->Start(err)) {
        m_inspect_overlay = ov;
    } else {
        delete ov;
        m_inspect_failed = true;
    }
}

int DepartureSweepController::PollInspect(RouteMapOverlay** grib_slot) {
    if (!m_inspect_overlay) return m_inspect_failed ? -1 : 0;
    if (m_inspect_done)     return 1;

    RouteMapOverlay* ov = m_inspect_overlay;

    if (ov->NeedsGrib() && !ov->Finished()) {
        if (grib_slot) *grib_slot = ov;
        ov->RequestGrib(ov->NewTime());
        if (grib_slot) *grib_slot = nullptr;
    }

    if (!ov->Running()) {
        ov->DeleteThread();
        wxString err = ov->GetError();
        if (!err.empty() || !ov->EndTime().IsValid()) {
            m_inspect_failed = true;
            delete m_inspect_overlay;
            m_inspect_overlay = nullptr;
            return -1;
        }
        m_inspect_done = true;
        return 1;
    }
    return 0;
}

bool DepartureSweepController::IsInspecting() const {
    return m_inspect_overlay != nullptr && !m_inspect_done && !m_inspect_failed;
}

void DepartureSweepController::FreeInspectOverlay() {
    if (!m_inspect_overlay) return;
    if (!m_inspect_done) {
        m_inspect_overlay->Stop();
        m_inspect_overlay->DeleteThread();
    }
    delete m_inspect_overlay;
    m_inspect_overlay = nullptr;
    m_inspect_done    = false;
    m_inspect_failed  = false;
}

#else  // UNIT_TESTS — stub implementations (no RouteMapOverlay dependency)

void DepartureSweepController::StartSweep(const SweepConfig& cfg, RouteMapOverlay**) {
    (void)cfg;
    m_running         = false;
    m_cancelled       = false;
    m_completed_count = 0;
    m_next_dispatch   = 0;
}
void DepartureSweepController::StopSweep() { m_cancelled = true; }
int  DepartureSweepController::Poll(RouteMapOverlay**) { m_running = false; return 0; }
void DepartureSweepController::ReapplyArrivalFilter(
    const ArrivalWindowParams&, const RankParams&) {}
void DepartureSweepController::FreeOverlays() {
    m_running_list.clear();
    m_candidates.clear();
    m_running         = false;
    m_cancelled       = false;
    m_completed_count = 0;
    m_next_dispatch   = 0;
    FreeInspectOverlay();
}
void DepartureSweepController::StartInspect(const wxDateTime&, RouteMapOverlay**) {}
int  DepartureSweepController::PollInspect(RouteMapOverlay**) { return 0; }
bool DepartureSweepController::IsInspecting() const { return false; }
void DepartureSweepController::FreeInspectOverlay() {
    m_inspect_overlay = nullptr;
    m_inspect_done    = false;
    m_inspect_failed  = false;
}
void DepartureSweepController::DispatchNext() {}
void DepartureSweepController::CollectCandidate(RouteMapOverlay*, size_t) {}

#endif // UNIT_TESTS
