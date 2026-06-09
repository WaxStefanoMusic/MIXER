#include "MixerConfig.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace mixer::config {

void MixerCfg::normalize()
{
    const size_t nb = buses.size();
    for (auto& s : strips)
    {
        s.routes.resize(nb, false);
        // Default invio al 100% per le rotte nuove / preset legacy.
        s.route_level.resize(nb, 1.0f);
    }
}

MixerCfg makeDefaultConfig()
{
    MixerCfg c;
    c.version = 1;

    StripCfg s;
    s.label = "Strip 1"; c.strips.push_back(s);
    s.label = "Strip 2"; c.strips.push_back(s);
    s.label = "Strip 3"; c.strips.push_back(s);
    s.label = "Strip 4"; c.strips.push_back(s);
    s.label = "Strip 5"; c.strips.push_back(s);

    BusCfg b;
    b.label = "Bus 1"; c.buses.push_back(b);
    b.label = "Bus 2"; c.buses.push_back(b);
    b.label = "Bus 3"; c.buses.push_back(b);

    c.normalize();
    return c;
}

// ----- wstring  utf-8 utilities ---------------------------------------------

static std::string wToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                          s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring utf8ToW(const std::string& s)
{
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                  nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string trim(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    size_t b = s.size();
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) --b;
    return s.substr(a, b - a);
}

// ----- save -----------------------------------------------------------------

bool saveToFile(const std::wstring& path, const MixerCfg& cfg)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    f << "# MIXER preset file\n";
    f << "version=" << cfg.version << "\n";
    f << "strips=" << cfg.strips.size() << "\n";
    f << "buses="  << cfg.buses.size()  << "\n\n";

    for (size_t i = 0; i < cfg.strips.size(); ++i)
    {
        const auto& s = cfg.strips[i];
        f << "strip." << i << ".label="    << s.label << "\n";
        f << "strip." << i << ".device="   << wToUtf8(s.device_id) << "\n";
        f << "strip." << i << ".loopback=" << (s.loopback ? "1" : "0") << "\n";
        f << "strip." << i << ".gain_db="  << s.gain_db << "\n";
        f << "strip." << i << ".mute="     << (s.mute ? "1" : "0") << "\n";
        f << "strip." << i << ".solo="     << (s.solo ? "1" : "0") << "\n";
        for (size_t b = 0; b < s.routes.size(); ++b)
            f << "strip." << i << ".route." << b << "=" << (s.routes[b] ? "1" : "0") << "\n";
        for (size_t b = 0; b < s.route_level.size(); ++b)
            f << "strip." << i << ".route." << b << ".level=" << s.route_level[b] << "\n";
        f << "\n";
    }

    for (size_t i = 0; i < cfg.buses.size(); ++i)
    {
        const auto& b = cfg.buses[i];
        f << "bus." << i << ".label="   << b.label << "\n";
        f << "bus." << i << ".device="  << wToUtf8(b.device_id) << "\n";
        f << "bus." << i << ".gain_db=" << b.gain_db << "\n";
        f << "bus." << i << ".mute="    << (b.mute ? "1" : "0") << "\n\n";
    }

    return f.good();
}

// ----- load -----------------------------------------------------------------

namespace {
bool starts_with(const std::string& s, const char* p)
{
    return std::strncmp(s.c_str(), p, std::strlen(p)) == 0;
}
int parseInt(const std::string& v, int def = 0)
{
    try { return std::stoi(v); } catch (...) { return def; }
}
float parseFloat(const std::string& v, float def = 0.0f)
{
    try { return std::stof(v); } catch (...) { return def; }
}
bool parseBool(const std::string& v) { return v == "1" || v == "true"; }
} // namespace

bool loadFromFile(const std::wstring& path, MixerCfg& out_cfg)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    MixerCfg c;
    c.version = 1;
    int declared_strips = 0;
    int declared_buses  = 0;

    auto ensureStrip = [&](size_t i) -> StripCfg& {
        while (c.strips.size() <= i) c.strips.emplace_back();
        return c.strips[i];
    };
    auto ensureBus = [&](size_t i) -> BusCfg& {
        while (c.buses.size() <= i) c.buses.emplace_back();
        return c.buses[i];
    };

    std::string line;
    while (std::getline(f, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "version")      { c.version = parseInt(val, 1); continue; }
        if (key == "strips")       { declared_strips = parseInt(val); continue; }
        if (key == "buses")        { declared_buses  = parseInt(val); continue; }

        if (starts_with(key, "strip."))
        {
            // strip.{i}.{prop}[.{n}]
            const char* p = key.c_str() + 6;
            char* end = nullptr;
            long i = std::strtol(p, &end, 10);
            if (i < 0 || !end || *end != '.') continue;
            std::string rest = end + 1;
            StripCfg& s = ensureStrip((size_t)i);
            if      (rest == "label")    s.label = val;
            else if (rest == "device")   s.device_id = utf8ToW(val);
            else if (rest == "loopback") s.loopback  = parseBool(val);
            else if (rest == "gain_db")  s.gain_db   = parseFloat(val);
            else if (rest == "mute")     s.mute      = parseBool(val);
            else if (rest == "solo")     s.solo      = parseBool(val);
            else if (starts_with(rest, "route."))
            {
                // "route.{b}" = on/off (bool), "route.{b}.level" = volume 0..1.
                char* rend = nullptr;
                long b = std::strtol(rest.c_str() + 6, &rend, 10);
                if (b < 0) continue;
                if (rend && std::strcmp(rend, ".level") == 0)
                {
                    if ((size_t)b >= s.route_level.size())
                        s.route_level.resize(b + 1, 1.0f);
                    s.route_level[b] =
                        std::clamp(parseFloat(val, 1.0f), 0.0f, 1.0f);
                }
                else
                {
                    if ((size_t)b >= s.routes.size())
                        s.routes.resize(b + 1, false);
                    s.routes[b] = parseBool(val);
                }
            }
            continue;
        }
        if (starts_with(key, "bus."))
        {
            const char* p = key.c_str() + 4;
            char* end = nullptr;
            long i = std::strtol(p, &end, 10);
            if (i < 0 || !end || *end != '.') continue;
            std::string rest = end + 1;
            BusCfg& b = ensureBus((size_t)i);
            if      (rest == "label")   b.label     = val;
            else if (rest == "device")  b.device_id = utf8ToW(val);
            else if (rest == "gain_db") b.gain_db   = parseFloat(val);
            else if (rest == "mute")    b.mute      = parseBool(val);
            continue;
        }
    }

    if (declared_strips > 0 && (int)c.strips.size() < declared_strips)
        c.strips.resize(declared_strips);
    if (declared_buses > 0 && (int)c.buses.size() < declared_buses)
        c.buses.resize(declared_buses);
    if (c.buses.empty() && c.strips.empty()) return false;

    c.normalize();
    out_cfg = std::move(c);
    return true;
}

} // namespace mixer::config
