/*
 * ComfortProfileLoader.cpp — WR.4 WindTrack Comfort Profile
 *
 * Contract: docs/DEPARTURE_SWEEP_CONTRACT_FINAL-1.md §6.
 */
#include "ComfortProfileLoader.h"

#include <json/json.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <limits>

// ---------------------------------------------------------------------------
// Load and validate
// ---------------------------------------------------------------------------

bool ComfortProfileLoader::Load(const std::string& path, std::string& error_msg) {
    m_loaded  = false;
    m_expired = false;
    m_profile_id.clear();
    m_input_features.clear();
    m_params.clear();

    std::ifstream f(path);
    if (!f.is_open()) {
        error_msg = "Cannot open file: " + path;
        return false;
    }

    Json::Value  root;
    Json::Reader reader;
    if (!reader.parse(f, root, false)) {
        error_msg = "JSON parse error: " + reader.getFormattedErrorMessages();
        return false;
    }

    if (!root.isMember("schema_version") || root["schema_version"].asInt() != 1) {
        error_msg = "Invalid profile: schema_version must be 1";
        return false;
    }
    if (!root.isMember("model_kind") ||
        root["model_kind"].asString() != "threshold_bins") {
        error_msg = "Invalid profile: model_kind must be \"threshold_bins\"";
        return false;
    }

    const Json::Value& features = root["input_features"];
    const Json::Value& params   = root["parameters"];
    if (!features.isArray()) {
        error_msg = "Invalid profile: input_features must be an array";
        return false;
    }
    if (!params.isArray() && !params.isObject()) {
        error_msg = "Invalid profile: parameters must be an array or object";
        return false;
    }
    if (params.isArray() && features.size() != params.size()) {
        error_msg = "Invalid profile: parameters length != input_features length";
        return false;
    }

    // Helper lambda: look up a feature's JSON node whether parameters is an
    // array (indexed parallel to input_features) or an object (keyed by name).
    auto getParam = [&](Json::ArrayIndex i) -> const Json::Value& {
        if (params.isArray()) return params[i];
        return params[features[i].asString()];
    };

    std::vector<ComfortFeature> tmp;
    tmp.reserve(features.size());
    for (Json::ArrayIndex i = 0; i < features.size(); ++i) {
        ComfortFeature cf;
        cf.name = features[i].asString();
        if (params.isObject() && !params.isMember(cf.name)) {
            error_msg = "Invalid profile: parameters missing key '" + cf.name + "'";
            return false;
        }
        const Json::Value& entry    = getParam(i);
        const Json::Value& edges    = entry["edges"];
        const Json::Value& penalties = entry["penalties"];
        if (!edges.isArray() || !penalties.isArray()) {
            error_msg = "Invalid profile: feature missing edges or penalties array";
            return false;
        }
        if (penalties.size() != edges.size() + 1) {
            error_msg = "Invalid profile: len(penalties) != len(edges) + 1";
            return false;
        }
        for (Json::ArrayIndex j = 0; j < edges.size(); ++j)
            cf.edges.push_back(edges[j].asDouble());
        for (Json::ArrayIndex j = 0; j < penalties.size(); ++j) {
            double p = penalties[j].asDouble();
            if (p < 0.0 || p > 1.0) {
                error_msg = "Invalid profile: penalty out of range [0, 1]";
                return false;
            }
            cf.penalties.push_back(p);
        }
        for (size_t j = 1; j < cf.edges.size(); ++j) {
            if (cf.edges[j] <= cf.edges[j - 1]) {
                error_msg = "Invalid profile: edges not strictly ascending";
                return false;
            }
        }
        tmp.push_back(std::move(cf));
    }

    m_input_features.clear();
    for (Json::ArrayIndex i = 0; i < features.size(); ++i)
        m_input_features.push_back(features[i].asString());
    m_params = std::move(tmp);

    m_profile_id = root.get("profile_id", "unknown").asString();

    std::string vu = root.get("valid_until", "").asString();
    if (!vu.empty())
        m_expired = CheckExpired(vu);

    m_loaded = true;
    return true;
}

std::string ComfortProfileLoader::StatusLabel() const {
    if (!m_loaded) return "";
    std::string s = "Loaded: " + m_profile_id;
    if (m_expired) s += " (expired)";
    return s;
}

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------

double ComfortProfileLoader::Score(const std::vector<WaypointSample>& samples) const {
    if (!m_loaded || samples.empty())
        return std::numeric_limits<double>::quiet_NaN();

    double total_wp = 0.0;
    int    n_wp     = 0;

    for (const WaypointSample& s : samples) {
        double wp_sum = 0.0;
        int    n_feat = 0;

        for (const ComfortFeature& feat : m_params) {
            double x = std::numeric_limits<double>::quiet_NaN();
            if      (feat.name == "tws_kn")  x = s.tws_kn;
            else if (feat.name == "twa_deg") x = s.twa_deg;
            else if (feat.name == "wvht_m")  x = std::isnan(s.wvht_m)  ? 0.0 : s.wvht_m;
            else if (feat.name == "wvper_s") x = std::isnan(s.wvper_s) ? 0.0 : s.wvper_s;
            else continue; // unknown feature — skip

            int bin = BinIndex(x, feat.edges);
            wp_sum += feat.penalties[bin];
            ++n_feat;
        }

        if (n_feat > 0) {
            total_wp += wp_sum / n_feat;
            ++n_wp;
        }
    }

    if (n_wp == 0)
        return std::numeric_limits<double>::quiet_NaN();

    double penalty = total_wp / n_wp;
    if (penalty < 0.0) penalty = 0.0;
    if (penalty > 1.0) penalty = 1.0;
    return penalty;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int ComfortProfileLoader::BinIndex(double x, const std::vector<double>& edges) {
    if (edges.empty() || x < edges[0]) return 0;
    if (x >= edges.back()) return (int)edges.size();
    // Find last index where edges[i] <= x.
    int lo = 0, hi = (int)edges.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (edges[mid] <= x) lo = mid;
        else                  hi = mid - 1;
    }
    return lo + 1;
}

bool ComfortProfileLoader::CheckExpired(const std::string& valid_until) {
    if (valid_until.empty()) return false;
    int yr = 0, mo = 0, dy = 0;
    if (std::sscanf(valid_until.c_str(), "%d-%d-%d", &yr, &mo, &dy) != 3)
        return false;
    std::time_t now = std::time(nullptr);
    struct std::tm t = {};
    t.tm_year = yr - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = dy;
    t.tm_hour = 23; t.tm_min = 59; t.tm_sec = 59;
    std::time_t vu = std::mktime(&t);
    return vu != (std::time_t)-1 && now > vu;
}
