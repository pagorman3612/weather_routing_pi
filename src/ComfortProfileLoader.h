/*
 * ComfortProfileLoader.h — WR.4 WindTrack Comfort Profile
 *
 * No wx dependency, no OpenCPN API dependency.
 * Uses jsoncpp (already a WRPI dependency).
 * Algorithm: threshold_bins schema v1 per contract §6.
 */
#pragma once
#include <cmath>
#include <limits>
#include <string>
#include <vector>

struct WaypointSample {
    double tws_kn  = 0.0;
    double twa_deg = 0.0;
    double wvht_m  = std::numeric_limits<double>::quiet_NaN();
    double wvper_s = std::numeric_limits<double>::quiet_NaN();
};

struct ComfortFeature {
    std::string          name;
    std::vector<double>  edges;     // strictly ascending; n elements
    std::vector<double>  penalties; // n+1 elements, values in [0, 1]
};

class ComfortProfileLoader {
public:
    // Load and validate a profile from a JSON file.
    // Returns true on success; sets error_msg on failure.
    bool Load(const std::string& path, std::string& error_msg);

    // Returns "Loaded: <id>" or "Loaded: <id> (expired)" on success; else "".
    std::string StatusLabel() const;

    bool IsLoaded()  const { return m_loaded; }
    bool IsExpired() const { return m_expired; }

    const std::vector<std::string>& InputFeatures() const { return m_input_features; }

    // Score a route as mean-over-waypoints of mean-over-features penalties.
    // Returns NaN when the sample list is empty or the profile is not loaded.
    double Score(const std::vector<WaypointSample>& samples) const;

private:
    static int  BinIndex(double x, const std::vector<double>& edges);
    static bool CheckExpired(const std::string& valid_until);

    bool        m_loaded  = false;
    bool        m_expired = false;
    std::string m_profile_id;
    std::vector<std::string>    m_input_features;
    std::vector<ComfortFeature> m_params;
};
