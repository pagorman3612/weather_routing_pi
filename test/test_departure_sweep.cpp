/*
 * test_departure_sweep.cpp — WR.1 + WR.2 + WR.4 unit tests
 *
 * Contract: docs/DEPARTURE_SWEEP_CONTRACT_FINAL-1.md §9.1
 */
#include <wx/wx.h>
#include <gtest/gtest.h>
#include <ostream>

// Prevent GTest from using ContainerPrinter<wxString> (which requires
// wxString::const_iterator::~const_iterator from the wx DLL and fails to link).
void PrintTo(const wxString& s, ::std::ostream* os) {
    *os << s.utf8_str().data();
}

#include <cmath>
#include <cstdio>
#include <fstream>
#include <list>
#include <string>
#include <vector>

#include "../src/DepartureSweepController.h"
#include "../src/SolarCalculator.h"
#include "../src/ComfortProfileLoader.h"

// ---------------------------------------------------------------------------
// Helper: write a string to a temp file, return the path.
// ---------------------------------------------------------------------------
static std::string WriteTempJson(const std::string& content) {
    char buf[L_tmpnam];
    std::tmpnam(buf);
    std::string path = std::string(buf) + ".json";
    std::ofstream f(path);
    f << content;
    return path;
}

// Helper: UTC wxDateTime from components.
static wxDateTime UTC(int year, int month, int day, int hour = 0, int min = 0) {
    return wxDateTime(day, static_cast<wxDateTime::Month>(month - 1), year, hour, min, 0, 0);
}

// Helper: construct a SweepCandidate for ranking tests.
static SweepCandidate MakeCandidate(bool succeeded, bool arrival_ok,
                                    double avg_tws, double duration_h,
                                    double comfort = std::numeric_limits<double>::quiet_NaN())
{
    SweepCandidate c;
    c.succeeded       = succeeded;
    c.arrival_ok      = arrival_ok;
    c.avg_tws_kn      = avg_tws;
    c.comfort_penalty = comfort;
    c.duration        = wxTimeSpan::Minutes(static_cast<long>(duration_h * 60));
    c.departure_utc   = UTC(2026, 6, 15, 0, 0);
    return c;
}

// Helper: build a PlotData point with given TWS, WVHT, and TWD/COG.
static PlotData MakePlot(double tws, double wvht = 0.0, double twd = 180.0, double cog = 180.0) {
    PlotData p;
    p.twsOverGround = tws;
    p.WVHT          = wvht;
    p.twdOverGround = twd;
    p.cog           = cog;
    return p;
}

// ============================================================
// test_departure_sweep_ranking
// ============================================================
TEST(DepartureSweepRanking, AllModes) {
    // 3 succeeded candidates with varying duration / TWS.
    // Candidate A: 10h, avg_tws=15
    // Candidate B: 8h, avg_tws=20
    // Candidate C: 12h, avg_tws=10
    // Plus 1 failed.
    std::vector<SweepCandidate> candidates;
    auto a = MakeCandidate(true,  true,  15.0, 10.0); a.departure_utc = UTC(2026,6,15,0,0);
    auto b = MakeCandidate(true,  true,  20.0,  8.0); b.departure_utc = UTC(2026,6,15,3,0);
    auto c = MakeCandidate(true,  true,  10.0, 12.0); c.departure_utc = UTC(2026,6,15,6,0);
    auto f = MakeCandidate(false, true,   0.0,  0.0); f.departure_utc = UTC(2026,6,15,9,0);
    candidates = {a, b, c, f};

    // FASTEST: B(8h) < A(10h) < C(12h).
    RankParams rp;
    rp.mode = SortMode::FASTEST;
    DepartureSweepController::RankCandidates(candidates, rp);
    ASSERT_EQ(candidates[0].rank, 1);
    EXPECT_DOUBLE_EQ(candidates[0].duration.GetMinutes(), 8*60);
    ASSERT_EQ(candidates[1].rank, 2);
    EXPECT_DOUBLE_EQ(candidates[1].duration.GetMinutes(), 10*60);
    ASSERT_EQ(candidates[2].rank, 3);
    EXPECT_DOUBLE_EQ(candidates[2].duration.GetMinutes(), 12*60);
    ASSERT_EQ(candidates[3].rank, 0);  // failed

    // SMOOTHEST (no profile): uses avg_tws proxy ascending.
    // C(10) < A(15) < B(20).
    candidates = {a, b, c, f};
    rp.mode = SortMode::SMOOTHEST;
    DepartureSweepController::RankCandidates(candidates, rp);
    EXPECT_NEAR(candidates[0].avg_tws_kn, 10.0, 1e-9);
    EXPECT_NEAR(candidates[1].avg_tws_kn, 15.0, 1e-9);
    EXPECT_NEAR(candidates[2].avg_tws_kn, 20.0, 1e-9);
    EXPECT_EQ(candidates[3].rank, 0);

    // All succeeded candidates rank above failed regardless of mode.
    for (int i = 0; i < 3; ++i)
        EXPECT_GT(candidates[i].rank, 0);
}

// ============================================================
// test_departure_sweep_ranking_balanced_weight
// ============================================================
TEST(DepartureSweepRanking, BalancedWeight) {
    // Two candidates: fast/rough vs slow/smooth.
    // B: 8h, avg_tws=20  → key_B = 8*(1+w*20/30) = 8+8w*0.667
    // C: 12h, avg_tws=10 → key_C = 12*(1+w*10/30) = 12+12w*0.333

    auto b = MakeCandidate(true, true, 20.0, 8.0);
    auto c = MakeCandidate(true, true, 10.0, 12.0);

    // w=0 → FASTEST: B(8h) wins.
    RankParams rp;
    rp.mode = SortMode::BALANCED;
    rp.balanced_weight = 0.0;
    std::vector<SweepCandidate> cands = {b, c};
    DepartureSweepController::RankCandidates(cands, rp);
    EXPECT_NEAR(cands[0].avg_tws_kn, 20.0, 1e-9); // B wins on duration

    // w=1 → comfort-heavier: key_B = 8*(1+0.667)=13.33; key_C = 12*(1+0.333)=16.0 → B still wins
    // To force C to win, use very high w and avg_tws difference big enough.
    // Reconstruct with extreme values: B=8h,tws=30; C=9h,tws=1.
    // key_B(w=1)= 8*(1+1.0)=16.0; key_C(w=1)= 9*(1+1/30)=9.3 → C wins with w=1.
    auto b2 = MakeCandidate(true, true, 30.0, 8.0);
    auto c2 = MakeCandidate(true, true,  1.0, 9.0);
    cands = {b2, c2};
    rp.balanced_weight = 0.0;
    DepartureSweepController::RankCandidates(cands, rp);
    EXPECT_NEAR(cands[0].avg_tws_kn, 30.0, 1e-9); // w=0: fastest (B) wins

    cands = {b2, c2};
    rp.balanced_weight = 1.0;
    DepartureSweepController::RankCandidates(cands, rp);
    EXPECT_NEAR(cands[0].avg_tws_kn, 1.0, 1e-9);  // w=1: smoothest (C) wins
}

// ============================================================
// test_sweep_candidate_window_clamp
// ============================================================
TEST(DepartureSweep, CandidateWindowClamp) {
    wxDateTime grib_start = UTC(2026, 6, 15,  0, 0);
    wxDateTime grib_end   = UTC(2026, 6, 20,  0, 0);

    // sweep extends beyond grib_end — should clamp.
    wxDateTime sw_start = UTC(2026, 6, 15, 0, 0);
    wxDateTime sw_end   = UTC(2026, 6, 22, 0, 0);
    auto r = DepartureSweepController::BuildCandidateList(
        sw_start, sw_end, grib_start, grib_end, 3*3600, 3600);
    EXPECT_TRUE(r.clamped_end);
    EXPECT_FALSE(r.clamped_start);
    EXPECT_FALSE(r.departures.empty());
    EXPECT_EQ(r.departures.back(), grib_end);

    // sweep starts before grib_start — should clamp.
    wxDateTime sw_start2 = UTC(2026, 6, 13, 0, 0);
    wxDateTime sw_end2   = UTC(2026, 6, 17, 0, 0);
    auto r2 = DepartureSweepController::BuildCandidateList(
        sw_start2, sw_end2, grib_start, grib_end, 6*3600, 3600);
    EXPECT_TRUE(r2.clamped_start);
    EXPECT_EQ(r2.departures.front(), grib_start);

    // step_s < delta_time_s → error, empty list.
    auto r3 = DepartureSweepController::BuildCandidateList(
        sw_start, grib_end, grib_start, grib_end, 1800, 3600);
    EXPECT_TRUE(r3.departures.empty());
    EXPECT_FALSE(r3.warning.empty());
}

// ============================================================
// test_arrival_filter_fixed_window
// ============================================================
TEST(ArrivalFilter, FixedWindow) {
    ArrivalWindowParams p;
    p.enabled              = true;
    p.mode                 = ArrivalFilterMode::FIXED_WINDOW;
    p.arrival_earliest_utc = 360;  // 06:00 UTC
    p.arrival_latest_utc   = 1200; // 20:00 UTC
    p.display_tz_offset_min = 0;

    wxString miss;

    // ETA at 12:00 UTC — inside window.
    EXPECT_TRUE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,12,0), p, miss));
    EXPECT_TRUE(miss.IsEmpty());

    // ETA at 03:00 UTC — outside window.
    EXPECT_FALSE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,3,0), p, miss));
    EXPECT_FALSE(miss.IsEmpty());

    // ETA at 06:00 UTC (earliest boundary) — inside.
    EXPECT_TRUE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,6,0), p, miss));

    // Wrap-midnight window: 22:00–04:00 UTC.
    p.arrival_earliest_utc = 22*60; // 22:00
    p.arrival_latest_utc   = 4*60;  // 04:00

    // ETA 23:00 — inside (>= 22:00).
    EXPECT_TRUE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,23,0), p, miss));

    // ETA 03:00 — inside (<= 04:00).
    EXPECT_TRUE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,3,0), p, miss));

    // ETA 12:00 — outside.
    EXPECT_FALSE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,12,0), p, miss));
    EXPECT_FALSE(miss.IsEmpty());

    // Filter disabled → always ok.
    p.enabled = false;
    EXPECT_TRUE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,3,0), p, miss));
}

// ============================================================
// test_arrival_filter_before_sunset
// ============================================================
TEST(ArrivalFilter, BeforeSunset) {
    // Southampton, UK (50.9°N, 1.4°W). Sunset ~21:10 UTC on 2026-06-21 (summer solstice).
    ArrivalWindowParams p;
    p.enabled              = true;
    p.mode                 = ArrivalFilterMode::BEFORE_SUNSET;
    p.sunset_margin_h      = 0.0;
    p.display_tz_offset_min = 0;
    p.end_lat              = 50.9;
    p.end_lon              = -1.4;

    wxString miss;

    // ETA at 15:00 UTC — well before sunset → ok.
    EXPECT_TRUE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,21,15,0), p, miss));

    // ETA at 23:00 UTC — after sunset → not ok.
    EXPECT_FALSE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,21,23,0), p, miss));
    EXPECT_FALSE(miss.IsEmpty());

    // 2-hour margin: ETA must be before sunset-2h.
    p.sunset_margin_h = 2.0;
    // ETA at 18:00 UTC — ~3h before sunset (before deadline) → ok.
    EXPECT_TRUE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,21,18,0), p, miss));
    // ETA at 20:00 UTC — only 1h before sunset (after deadline) → not ok.
    EXPECT_FALSE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,21,20,0), p, miss));

    // Filter disabled → always ok even after sunset.
    p.enabled = false;
    p.sunset_margin_h = 0.0;
    EXPECT_TRUE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,21,23,0), p, miss));
}

// ============================================================
// test_arrival_filter_polar_cases
// ============================================================
TEST(ArrivalFilter, PolarCases) {
    // Midnight sun: Tromsø, Norway (69.6°N, 18.9°E) on 2026-06-21.
    ArrivalWindowParams p;
    p.enabled              = true;
    p.mode                 = ArrivalFilterMode::BEFORE_SUNSET;
    p.sunset_margin_h      = 0.0;
    p.display_tz_offset_min = 0;
    p.end_lat              = 69.6;
    p.end_lon              = 18.9;

    wxString miss;

    // Midnight sun: always arrival_ok = true.
    EXPECT_TRUE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,21,23,0), p, miss));
    EXPECT_TRUE(miss.IsEmpty());

    // Polar night: Tromsø on 2026-12-15.
    // Sunset returns invalid + polar_night=true → arrival_ok = false.
    EXPECT_FALSE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,12,15,12,0), p, miss));
    EXPECT_NE(miss.Find("Polar night"), wxNOT_FOUND);
}

// ============================================================
// test_solar_calculator_known_values
// ============================================================
TEST(SolarCalculator, KnownValues) {
    // Reference: NOAA Solar Calculator (UTC times, not local).
    // London (51.5°N, 0°W), 2026-06-21 (summer solstice):
    //   local sunset ~21:22 BST (UTC+1) = ~20:22 UTC = 1222 min. Allow ±15 min.
    {
        wxDateTime sunset = SolarCalculator::Sunset(2026, 6, 21, 51.5, 0.0);
        ASSERT_TRUE(sunset.IsValid());
        int sunset_min = sunset.GetHour() * 60 + sunset.GetMinute();
        EXPECT_GE(sunset_min, 1207);
        EXPECT_LE(sunset_min, 1237);
    }

    // Sydney, Australia (−33.87°S, 151.21°E), 2026-06-21 (winter):
    //   local sunset ~17:04 AEST (UTC+10) = ~07:04 UTC = 424 min. Allow ±20 min.
    {
        wxDateTime sunset = SolarCalculator::Sunset(2026, 6, 21, -33.87, 151.21);
        ASSERT_TRUE(sunset.IsValid());
        int sunset_min = sunset.GetHour() * 60 + sunset.GetMinute();
        EXPECT_GE(sunset_min, 404);
        EXPECT_LE(sunset_min, 444);
    }

    // Equator (0°, 0°E), 2026-03-20 (equinox):
    //   sunset ~18:07 UTC accounting for refraction = 1087 min. Allow ±15 min.
    {
        wxDateTime sunset = SolarCalculator::Sunset(2026, 3, 20, 0.0, 0.0);
        ASSERT_TRUE(sunset.IsValid());
        int sunset_min = sunset.GetHour() * 60 + sunset.GetMinute();
        EXPECT_GE(sunset_min, 1072);
        EXPECT_LE(sunset_min, 1102);
    }

    // Midnight sun: Tromsø (69.6°N) on 2026-06-21.
    {
        bool polar_night = false;
        wxDateTime sunset = SolarCalculator::Sunset(2026, 6, 21, 69.6, 18.9, &polar_night);
        EXPECT_FALSE(polar_night);
        EXPECT_FALSE(sunset.IsValid()); // midnight sun → invalid
    }

    // Polar night: Tromsø (69.6°N) on 2026-12-15.
    {
        bool polar_night = false;
        wxDateTime sunset = SolarCalculator::Sunset(2026, 12, 15, 69.6, 18.9, &polar_night);
        EXPECT_TRUE(polar_night);
        EXPECT_FALSE(sunset.IsValid());
    }
}

// ============================================================
// test_ranking_three_tier
// ============================================================
TEST(DepartureSweepRanking, ThreeTier) {
    auto t1a = MakeCandidate(true,  true,  10.0, 10.0); t1a.departure_utc = UTC(2026,6,15,0,0);
    auto t1b = MakeCandidate(true,  true,  15.0,  8.0); t1b.departure_utc = UTC(2026,6,15,3,0);
    auto t2a = MakeCandidate(true,  false, 12.0, 11.0); t2a.departure_utc = UTC(2026,6,15,6,0);
    auto t2b = MakeCandidate(true,  false,  9.0, 12.0); t2b.departure_utc = UTC(2026,6,15,9,0);
    auto t3a = MakeCandidate(false, true,   0.0,  0.0); t3a.departure_utc = UTC(2026,6,15,6,0);
    auto t3b = MakeCandidate(false, true,   0.0,  0.0); t3b.departure_utc = UTC(2026,6,15,3,0);
    std::vector<SweepCandidate> cands = {t2a, t3a, t1a, t2b, t1b, t3b};

    RankParams rp;
    rp.mode = SortMode::FASTEST;
    DepartureSweepController::RankCandidates(cands, rp);

    // All Tier 1 rank before any Tier 2.
    // All Tier 2 rank before any Tier 3.
    // Tier 3 has rank=0.
    int last_t1_rank = -1, last_t2_rank = -1;
    for (const auto& c : cands) {
        if (c.succeeded && c.arrival_ok) {
            last_t1_rank = c.rank;
            EXPECT_GT(c.rank, 0);
        }
    }
    for (const auto& c : cands) {
        if (c.succeeded && !c.arrival_ok) {
            last_t2_rank = c.rank;
            EXPECT_GT(c.rank, last_t1_rank); // Tier 2 ranks higher number than any Tier 1
        }
    }
    for (const auto& c : cands) {
        if (!c.succeeded) {
            EXPECT_EQ(c.rank, 0);
        }
    }

    // Rank sequence is continuous across Tier 1+2 combined.
    int expected = 1;
    for (const auto& c : cands) {
        if (c.succeeded) {
            EXPECT_EQ(c.rank, expected++);
        }
    }
}

// ============================================================
// test_display_tz_formatting
// ============================================================
TEST(DisplayTz, Formatting) {
    wxDateTime base = UTC(2026, 6, 15, 14, 0); // 2026-06-15 14:00 UTC

    // UTC+0: no change.
    EXPECT_EQ(DepartureSweepController::FormatDisplayTime(base, 0),
              wxString("2026-06-15 14:00"));

    // UTC+9:30 (570 min): 14:00 + 9:30 = 23:30.
    EXPECT_EQ(DepartureSweepController::FormatDisplayTime(base, 570),
              wxString("2026-06-15 23:30"));

    // UTC-5:00 (−300 min): 14:00 - 5:00 = 09:00.
    EXPECT_EQ(DepartureSweepController::FormatDisplayTime(base, -300),
              wxString("2026-06-15 09:00"));
}

// ============================================================
// test_display_tz_arrival_filter_fixed
// ============================================================
TEST(DisplayTz, ArrivalFilterFixed) {
    // User enters window 06:00–20:00 at UTC+9:30 (570 min).
    // Stored UTC: 06:00 - 9:30 = 20:30 previous day → mod 1440 = 1230 min
    //             20:00 - 9:30 = 10:30 = 630 min
    // So stored earliest=1230, latest=630 (wrap-midnight UTC window).
    int tz = 570;
    int entered_early_disp = 6*60;   // 06:00 display
    int entered_late_disp  = 20*60;  // 20:00 display
    int stored_early = ((entered_early_disp - tz) % 1440 + 1440) % 1440; // = 1230
    int stored_late  = ((entered_late_disp  - tz) % 1440 + 1440) % 1440; // = 630

    EXPECT_EQ(stored_early, 1230);
    EXPECT_EQ(stored_late,  630);

    ArrivalWindowParams p;
    p.enabled               = true;
    p.mode                  = ArrivalFilterMode::FIXED_WINDOW;
    p.arrival_earliest_utc  = stored_early;
    p.arrival_latest_utc    = stored_late;
    p.display_tz_offset_min = tz;

    wxString miss;

    // ETA 22:00 UTC (= 07:30 local) — inside window (07:30 between 06:00 and 20:00 local).
    // UTC 22:00 = 1320 min: 1320 >= 1230 → inside wrap window → arrival_ok.
    EXPECT_TRUE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,22,0), p, miss));

    // ETA 14:00 UTC (= 23:30 local) — outside window.
    // UTC 14:00 = 840 min: 840 is NOT >= 1230 and NOT <= 630 → outside.
    EXPECT_FALSE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,14,0), p, miss));
    EXPECT_FALSE(miss.IsEmpty());
}

// ============================================================
// test_display_tz_miss_reason_label
// ============================================================
TEST(DisplayTz, MissReasonLabel) {
    // With UTC+3:00, miss-reason strings should say "UTC+3:00", not "UTC".
    ArrivalWindowParams p;
    p.enabled               = true;
    p.mode                  = ArrivalFilterMode::FIXED_WINDOW;
    p.arrival_earliest_utc  = 9*60;   // 09:00 UTC
    p.arrival_latest_utc    = 18*60;  // 18:00 UTC
    p.display_tz_offset_min = 180;    // UTC+3:00

    wxString miss;
    // ETA at 05:00 UTC (= 08:00 local) — outside window (window is 09:00–18:00 UTC).
    EXPECT_FALSE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,5,0), p, miss));
    EXPECT_NE(miss.Find("UTC+3:00"), wxNOT_FOUND);

    // Negative offset.
    p.display_tz_offset_min = -180; // UTC-3:00
    EXPECT_FALSE(DepartureSweepController::EvaluateArrivalWindow(UTC(2026,6,15,5,0), p, miss));
    EXPECT_NE(miss.Find("UTC-3:00"), wxNOT_FOUND);
}

// ============================================================
// test_peak_stats_extraction
// ============================================================
TEST(PeakStats, Extraction) {
    std::list<PlotData> chain;
    // TWS: 8, 15, 22, 12 → avg=14.25, max=22.
    // WVHT: 1.5, 2.0, 3.5, 2.5 → max=3.5.
    // All downwind (TWD=180°, COG=180°) → TWA=0° → all upwind (twa<60), fraction=1.0.
    chain.push_back(MakePlot(8.0,  1.5, 180.0, 180.0));
    chain.push_back(MakePlot(15.0, 2.0, 180.0, 180.0));
    chain.push_back(MakePlot(22.0, 3.5, 180.0, 180.0));
    chain.push_back(MakePlot(12.0, 2.5, 180.0, 180.0));

    double avg_tws, max_tws, max_swell, upwind;
    DepartureSweepController::ExtractPlotStats(chain, avg_tws, max_tws, max_swell, upwind);

    EXPECT_NEAR(avg_tws, 14.25, 1e-9);
    EXPECT_NEAR(max_tws, 22.0,  1e-9);
    EXPECT_NEAR(max_swell, 3.5, 1e-9);
    // TWD=180, COG=180 → HeadingDiff=0 → TWA=0 < 60 → upwind for all.
    EXPECT_NEAR(upwind, 1.0, 1e-9);

    // Empty chain → NaN.
    std::list<PlotData> empty;
    DepartureSweepController::ExtractPlotStats(empty, avg_tws, max_tws, max_swell, upwind);
    EXPECT_TRUE(std::isnan(avg_tws));
    EXPECT_TRUE(std::isnan(max_tws));
    EXPECT_TRUE(std::isnan(max_swell));
    EXPECT_TRUE(std::isnan(upwind));
}

// ============================================================
// test_peak_stats_no_swell
// ============================================================
TEST(PeakStats, NoSwell) {
    std::list<PlotData> chain;
    chain.push_back(MakePlot(10.0, 0.0)); // WVHT=0 → no swell data
    chain.push_back(MakePlot(20.0, 0.0));
    chain.push_back(MakePlot(15.0, 0.0));

    double avg_tws, max_tws, max_swell, upwind;
    DepartureSweepController::ExtractPlotStats(chain, avg_tws, max_tws, max_swell, upwind);

    EXPECT_NEAR(avg_tws, 15.0, 1e-9);
    EXPECT_NEAR(max_tws, 20.0, 1e-9);
    EXPECT_TRUE(std::isnan(max_swell)); // no swell data → NaN
    EXPECT_FALSE(std::isnan(upwind));
}

// ============================================================
// test_step_size_candidates
// ============================================================
TEST(DepartureSweep, StepSizeCandidates) {
    wxDateTime base  = UTC(2026, 6, 15, 0, 0);
    wxDateTime end12 = UTC(2026, 6, 15, 12, 0);

    // step=3h → candidates at 0, 3, 6, 9, 12 → 5 candidates.
    auto r1 = DepartureSweepController::BuildCandidateList(
        base, end12, base, end12, 3*3600, 3600);
    EXPECT_TRUE(r1.warning.IsEmpty());
    ASSERT_EQ(r1.departures.size(), 5u);
    EXPECT_EQ(r1.departures[0], UTC(2026,6,15,0,0));
    EXPECT_EQ(r1.departures[2], UTC(2026,6,15,6,0));
    EXPECT_EQ(r1.departures[4], UTC(2026,6,15,12,0));

    // step=6h → 0, 6, 12 → 3 candidates.
    auto r2 = DepartureSweepController::BuildCandidateList(
        base, end12, base, end12, 6*3600, 3600);
    EXPECT_TRUE(r2.warning.IsEmpty());
    ASSERT_EQ(r2.departures.size(), 3u);
    EXPECT_EQ(r2.departures[1], UTC(2026,6,15,6,0));

    // step < delta_time → error.
    auto r3 = DepartureSweepController::BuildCandidateList(
        base, end12, base, end12, 1800, 3600);
    EXPECT_TRUE(r3.departures.empty());
    EXPECT_FALSE(r3.warning.IsEmpty());
}

// ============================================================
// WR.4 — ComfortProfileLoader
// ============================================================

static const char* kProfile2Feature = R"({
    "schema_version": 1,
    "model_kind": "threshold_bins",
    "profile_id": "test_profile_2f",
    "input_features": ["tws_kn", "twa_deg"],
    "parameters": [
        { "edges": [10.0, 20.0], "penalties": [0.1, 0.5, 0.9] },
        { "edges": [30.0, 90.0], "penalties": [0.2, 0.4, 0.6] }
    ]
})";

static const char* kProfile4Feature = R"({
    "schema_version": 1,
    "model_kind": "threshold_bins",
    "profile_id": "test_profile_4f",
    "input_features": ["tws_kn", "twa_deg", "wvht_m", "wvper_s"],
    "parameters": [
        { "edges": [10.0, 20.0],    "penalties": [0.0, 0.5, 1.0] },
        { "edges": [45.0, 135.0],   "penalties": [0.0, 0.5, 1.0] },
        { "edges": [1.5, 3.0],      "penalties": [0.0, 0.5, 1.0] },
        { "edges": [6.0, 10.0],     "penalties": [0.0, 0.5, 1.0] }
    ]
})";

static const char* kProfileExpired = R"({
    "schema_version": 1,
    "model_kind": "threshold_bins",
    "profile_id": "expired_profile",
    "valid_until": "2020-01-01",
    "input_features": ["tws_kn"],
    "parameters": [
        { "edges": [15.0], "penalties": [0.2, 0.8] }
    ]
})";

// -- test_comfort_profile_loader_valid (2-feature) ----------------------------
TEST(ComfortProfileLoader, Valid2Feature) {
    std::string path = WriteTempJson(kProfile2Feature);
    ComfortProfileLoader ldr;
    std::string err;
    ASSERT_TRUE(ldr.Load(path, err)) << err;
    EXPECT_TRUE(ldr.IsLoaded());
    EXPECT_FALSE(ldr.IsExpired());
    EXPECT_NE(ldr.StatusLabel().find("test_profile_2f"), std::string::npos);

    // Score: tws=15 (bin 1, penalty 0.5), twa=45 (bin 1, penalty 0.4)
    // → waypoint mean = (0.5 + 0.4)/2 = 0.45
    std::vector<WaypointSample> s = {{ 15.0, 45.0 }};
    double score = ldr.Score(s);
    EXPECT_NEAR(score, 0.45, 1e-6);
    std::remove(path.c_str());
}

// -- test_comfort_profile_loader_valid (4-feature) ----------------------------
TEST(ComfortProfileLoader, Valid4Feature) {
    std::string path = WriteTempJson(kProfile4Feature);
    ComfortProfileLoader ldr;
    std::string err;
    ASSERT_TRUE(ldr.Load(path, err)) << err;

    // tws=25 (>=20, bin 2, pen 1.0), twa=90 (>=45 <135, bin 1, pen 0.5)
    // wvht=2.0 (>=1.5 <3.0, bin 1, pen 0.5), wvper=8.0 (>=6 <10, bin 1, pen 0.5)
    // waypoint mean = (1.0 + 0.5 + 0.5 + 0.5) / 4 = 0.625
    std::vector<WaypointSample> s = {{ 25.0, 90.0, 2.0, 8.0 }};
    EXPECT_NEAR(ldr.Score(s), 0.625, 1e-6);
    std::remove(path.c_str());
}

// -- test_comfort_profile_loader_invalid --------------------------------------
TEST(ComfortProfileLoader, InvalidSchemaVersion) {
    std::string path = WriteTempJson(R"({"schema_version":2,"model_kind":"threshold_bins","input_features":[],"parameters":[]})");
    ComfortProfileLoader ldr;
    std::string err;
    EXPECT_FALSE(ldr.Load(path, err));
    EXPECT_FALSE(err.empty());
    std::remove(path.c_str());
}

TEST(ComfortProfileLoader, InvalidModelKind) {
    std::string path = WriteTempJson(R"({"schema_version":1,"model_kind":"neural_net","input_features":[],"parameters":[]})");
    ComfortProfileLoader ldr;
    std::string err;
    EXPECT_FALSE(ldr.Load(path, err));
    std::remove(path.c_str());
}

TEST(ComfortProfileLoader, MismatchedPenaltiesLength) {
    // penalties has 2 elements but edges has 2 → need 3
    std::string path = WriteTempJson(R"({"schema_version":1,"model_kind":"threshold_bins",
        "input_features":["tws_kn"],"parameters":[{"edges":[10.0,20.0],"penalties":[0.1,0.5]}]})");
    ComfortProfileLoader ldr;
    std::string err;
    EXPECT_FALSE(ldr.Load(path, err));
    std::remove(path.c_str());
}

TEST(ComfortProfileLoader, NonAscendingEdges) {
    std::string path = WriteTempJson(R"({"schema_version":1,"model_kind":"threshold_bins",
        "input_features":["tws_kn"],"parameters":[{"edges":[20.0,10.0],"penalties":[0.1,0.5,0.9]}]})");
    ComfortProfileLoader ldr;
    std::string err;
    EXPECT_FALSE(ldr.Load(path, err));
    std::remove(path.c_str());
}

TEST(ComfortProfileLoader, OutOfRangePenalty) {
    std::string path = WriteTempJson(R"({"schema_version":1,"model_kind":"threshold_bins",
        "input_features":["tws_kn"],"parameters":[{"edges":[10.0],"penalties":[0.5,1.5]}]})");
    ComfortProfileLoader ldr;
    std::string err;
    EXPECT_FALSE(ldr.Load(path, err));
    std::remove(path.c_str());
}

// -- test_comfort_profile_loader_expired --------------------------------------
TEST(ComfortProfileLoader, Expired) {
    std::string path = WriteTempJson(kProfileExpired);
    ComfortProfileLoader ldr;
    std::string err;
    ASSERT_TRUE(ldr.Load(path, err)) << err; // expired still loads
    EXPECT_TRUE(ldr.IsLoaded());
    EXPECT_TRUE(ldr.IsExpired());
    EXPECT_NE(ldr.StatusLabel().find("(expired)"), std::string::npos);

    // Scoring still works even when expired.
    // tws=5 < 15 → bin 0, pen 0.2
    std::vector<WaypointSample> s = {{ 5.0, 0.0 }};
    EXPECT_NEAR(ldr.Score(s), 0.2, 1e-6);
    std::remove(path.c_str());
}

// -- Empty sample list → NaN --------------------------------------------------
TEST(ComfortProfileLoader, EmptySamples) {
    std::string path = WriteTempJson(kProfile2Feature);
    ComfortProfileLoader ldr;
    std::string err;
    ASSERT_TRUE(ldr.Load(path, err));
    std::vector<WaypointSample> empty;
    EXPECT_TRUE(std::isnan(ldr.Score(empty)));
    std::remove(path.c_str());
}

// -- BinIndex edge cases ------------------------------------------------------
TEST(ComfortProfileLoader, BinIndexEdgeCases) {
    // Via Score: verify boundary behaviour directly.
    // Profile with single edge at 15.0: penalty below=0.1, above=0.9
    std::string path = WriteTempJson(R"({"schema_version":1,"model_kind":"threshold_bins",
        "profile_id":"bin_test","input_features":["tws_kn"],
        "parameters":[{"edges":[15.0],"penalties":[0.1,0.9]}]})");
    ComfortProfileLoader ldr;
    std::string err;
    ASSERT_TRUE(ldr.Load(path, err));

    // x < 15 → bin 0 → 0.1
    EXPECT_NEAR(ldr.Score({{ 14.9, 0.0 }}), 0.1, 1e-6);
    // x == 15 → bin 1 → 0.9
    EXPECT_NEAR(ldr.Score({{ 15.0, 0.0 }}), 0.9, 1e-6);
    // x > 15 → bin 1 → 0.9
    EXPECT_NEAR(ldr.Score({{ 25.0, 0.0 }}), 0.9, 1e-6);
    std::remove(path.c_str());
}

// -- Multi-waypoint averaging -------------------------------------------------
TEST(ComfortProfileLoader, MultiWaypointAveraging) {
    std::string path = WriteTempJson(kProfile2Feature);
    ComfortProfileLoader ldr;
    std::string err;
    ASSERT_TRUE(ldr.Load(path, err));

    // wp1: tws=5 (bin0, pen 0.1), twa=20 (bin0, pen 0.2) → mean = 0.15
    // wp2: tws=25 (bin2, pen 0.9), twa=60 (bin1, pen 0.4) → mean = 0.65
    // overall = (0.15 + 0.65) / 2 = 0.40
    std::vector<WaypointSample> samples = {
        { 5.0, 20.0 },
        { 25.0, 60.0 }
    };
    EXPECT_NEAR(ldr.Score(samples), 0.40, 1e-6);
    std::remove(path.c_str());
}

// -- Object (map-keyed) parameters format — WindTrack actual export format ----
TEST(ComfortProfileLoader, ObjectFormatParameters) {
    // WindTrack exports parameters as a JSON object keyed by feature name,
    // not as an array. Verify the loader accepts both.
    static const char* kProfileObjectFormat = R"({
        "schema_version": 1,
        "model_kind": "threshold_bins",
        "profile_id": "object_format_test",
        "input_features": ["tws_kn", "twa_deg"],
        "parameters": {
            "tws_kn":  { "edges": [10.0, 20.0], "penalties": [0.1, 0.5, 0.9] },
            "twa_deg": { "edges": [45.0, 135.0], "penalties": [0.2, 0.4, 0.8] }
        }
    })";
    std::string path = WriteTempJson(kProfileObjectFormat);
    ComfortProfileLoader ldr;
    std::string err;
    ASSERT_TRUE(ldr.Load(path, err)) << err;
    EXPECT_TRUE(ldr.IsLoaded());
    EXPECT_NE(ldr.StatusLabel().find("object_format_test"), std::string::npos);

    // tws=15 (bin 1, pen 0.5), twa=45 (bin 1, pen 0.4) → mean = 0.45
    std::vector<WaypointSample> s = {{ 15.0, 45.0 }};
    EXPECT_NEAR(ldr.Score(s), 0.45, 1e-6);
    std::remove(path.c_str());
}
