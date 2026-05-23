/*
 * SolarCalculator.cpp — WR.2 Departure Sweep
 *
 * NOAA / US Naval Observatory sunset algorithm.
 * Reuses the same core maths as SunCalculator::CalculateSun() but wraps
 * it in the SolarCalculator::Sunset() API defined by the contract.
 */
#include <wx/wx.h>
#include "SolarCalculator.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

static const double DEG = M_PI / 180.0;
static const double RAD = 180.0 / M_PI;
static const double ZENITH = 90.0 + 50.0 / 60.0; // official zenith (refraction)

// Compute day-of-year (1-based) from year/month/day.
int DayOfYear(int year, int month, int day) {
    // Formula from US Naval Observatory algorithm.
    int N1 = (int)(275.0 * month / 9.0);
    int N2 = (int)((month + 9.0) / 12.0);
    int N3 = (int)(1.0 + (year - 4.0 * (int)(year / 4.0) + 2.0) / 3.0);
    return N1 - (N2 * N3) + day - 30;
}

// Steps 3-6: sun mean anomaly → right ascension, sin/cos declination.
void SunPosition(double t, double& sinDec, double& cosDec, double& RA) {
    double M = 0.9856 * t - 3.289;
    double L = M + 1.916 * std::sin(DEG * M) + 0.020 * std::sin(2.0 * DEG * M) + 282.634;
    while (L > 360.0) L -= 360.0;
    while (L < 0.0)   L += 360.0;
    RA = RAD * std::atan(0.91764 * std::tan(DEG * L));
    while (RA > 360.0) RA -= 360.0;
    while (RA < 0.0)   RA += 360.0;
    double Lq = std::floor(L / 90.0) * 90.0;
    double RAq = std::floor(RA / 90.0) * 90.0;
    RA = (RA + (Lq - RAq)) / 15.0; // hours
    sinDec = 0.39782 * std::sin(DEG * L);
    cosDec = std::cos(std::asin(sinDec));
}

} // namespace

wxDateTime SolarCalculator::Sunset(int year, int month, int day,
                                   double lat_deg, double lon_deg,
                                   bool* polar_night)
{
    if (polar_night) *polar_night = false;

    int N = DayOfYear(year, month, day);
    double lngHour = lon_deg / 15.0;
    double tset = N + (18.0 - lngHour) / 24.0;

    double sinDec, cosDec, RA;
    SunPosition(tset, sinDec, cosDec, RA);

    double cosZenith = std::cos(DEG * ZENITH);
    double cosH = (cosZenith - sinDec * std::sin(DEG * lat_deg))
                  / (cosDec * std::cos(DEG * lat_deg));

    if (cosH > 1.0) {
        // Sun never rises — polar night.
        if (polar_night) *polar_night = true;
        return wxInvalidDateTime;
    }
    if (cosH < -1.0) {
        // Sun never sets — midnight sun.
        return wxInvalidDateTime;
    }

    double H = RAD * std::acos(cosH);   // setting: H = acos(cosH)
    H /= 15.0;                           // convert to hours

    double T = H + RA - 0.06571 * tset - 6.622;
    double UT = T - lngHour;
    while (UT >= 24.0) UT -= 24.0;
    while (UT < 0.0)   UT += 24.0;

    int hr = (int)UT;
    int mn = (int)((UT - hr) * 60.0 + 0.5);
    if (mn >= 60) { mn -= 60; ++hr; }
    if (hr >= 24) hr -= 24;

    // hr/mn are UTC components; build as local and then neutralise the machine TZ.
    // GetOffset() returns standard-time offset on Windows (no DST), so we compute
    // the live offset from Now() to get the DST-adjusted value.
    wxDateTime dt(day, static_cast<wxDateTime::Month>(month - 1), year, hr, mn, 0, 0);
    wxDateTime t = wxDateTime::Now();
    long offSecs = ((long)t.GetHour() * 3600 + t.GetMinute() * 60 + t.GetSecond())
                 - ((long)t.GetHour(wxDateTime::UTC) * 3600
                    + t.GetMinute(wxDateTime::UTC) * 60 + t.GetSecond(wxDateTime::UTC));
    if (offSecs >  43200) offSecs -= 86400;
    if (offSecs < -43200) offSecs += 86400;
    dt += wxTimeSpan::Seconds(offSecs);
    return dt;
}
