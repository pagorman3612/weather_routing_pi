/*
 * DepartureSweepController.cpp — WR.1 + WR.2 Departure Sweep
 *
 * Contract: docs/DEPARTURE_SWEEP_CONTRACT_FINAL-1.md §3, §4.
 * Note: RouteMapOverlay integration is deferred to WR.3 (DeparturePlanningDialog).
 */
#include <wx/wx.h>
#include "DepartureSweepController.h"
#include "SolarCalculator.h"

#include <algorithm>
#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

static const double kNaN = std::numeric_limits<double>::quiet_NaN();

// Absolute angular difference between two compass headings, result in [0, 180].
double HeadingDiff(double a, double b) {
    double d = std::fabs(a - b);
    while (d >= 360.0) d -= 360.0;
    return d > 180.0 ? 360.0 - d : d;
}

// UTC time-of-day in minutes from midnight.
int EtaTod(const wxDateTime& eta_utc) {
    return eta_utc.GetHour() * 60 + eta_utc.GetMinute();
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
            miss_reason = wxString::Format("Arrives %s %s — outside window %s–%s %s",
                eta_disp, tz_label, win_early, win_late, tz_label);
        }
        return ok;
    }

    // Mode B — Before sunset at destination.
    if (std::isnan(params.end_lat) || std::isnan(params.end_lon)) {
        return true; // no destination coords — can't evaluate
    }

    bool polar_night = false;
    wxDateTime sunset = SolarCalculator::Sunset(
        eta_utc.GetYear(),
        static_cast<int>(eta_utc.GetMonth()) + 1,
        eta_utc.GetDay(),
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
    int deadline_tod = deadline.GetHour() * 60 + deadline.GetMinute();

    bool ok = (eta_tod <= deadline_tod);
    if (!ok) {
        wxString eta_disp = FormatDisplayTime(eta_utc, params.display_tz_offset_min, "%H:%M");
        wxString set_disp = FormatDisplayTime(deadline, params.display_tz_offset_min, "%H:%M");
        wxString margin_str;
        if (params.sunset_margin_h > 0.0)
            margin_str = wxString::Format(" (-%s h buffer)",
                wxString::Format("%.4g", params.sunset_margin_h));
        miss_reason = wxString::Format("Arrives %s %s — sunset%s %s %s",
            eta_disp, tz_label, margin_str, set_disp, tz_label);
    }
    return ok;
}

wxString DepartureSweepController::FormatDisplayTime(
    const wxDateTime& utc,
    int tz_offset_min,
    const wxString& fmt)
{
    if (!utc.IsValid()) return "—";
    wxDateTime local = utc + wxTimeSpan::Minutes(tz_offset_min);
    return local.Format(fmt);
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
