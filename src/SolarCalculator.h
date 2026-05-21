/*
 * SolarCalculator.h — WR.2 Departure Sweep
 *
 * Standalone sunset computation. No OpenCPN API dependency.
 * Uses wxDateTime for input/output only (wxBase, no UI).
 * Accuracy: ±5 minutes at latitudes below 70°.
 * Algorithm: NOAA / US Naval Observatory (Jean Meeus, Astronomical Algorithms 2e).
 */
#pragma once
#include <wx/wx.h>

class SolarCalculator {
public:
    // Returns UTC time of sunset on the given date at (lat_deg, lon_deg).
    //
    // Returns wxInvalidDateTime when the sun never sets (midnight sun).
    // Returns wxInvalidDateTime and sets *polar_night = true when the sun
    // never rises (polar night). polar_night is set false for all other cases.
    static wxDateTime Sunset(int year, int month, int day,
                             double lat_deg, double lon_deg,
                             bool* polar_night = nullptr);
};
