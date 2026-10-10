// A log aggregator with a deliberately mixed set of costs. Version chosen by -DV=1|2|3.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef V
#define V 1
#endif

[[gnu::noinline]] static std::string make_log(int lines) {
    std::mt19937 rng(7);
    std::string out;
    for (int i = 0; i < lines; ++i) {
        out += "user" + std::to_string(rng() % 5000) + ",GET,/item/" + std::to_string(rng() % 100000) + "," +
               std::to_string(rng() % 100000) + "\n";
    }
    return out;
}

#if V == 1
// v1: the "obvious" code
[[gnu::noinline]] static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) { if (c == sep) { parts.push_back(cur); cur.clear(); } else cur += c; }
    parts.push_back(cur);
    return parts;
}
[[gnu::noinline]] static std::vector<std::pair<std::string, long>> aggregate(const std::string& log) {
    std::map<std::string, long> total;
    std::istringstream in(log);
    std::string line;
    while (std::getline(in, line)) {
        auto f = split(line, ',');
        total[f[0]] += std::stol(f[3]);
    }
    return {total.begin(), total.end()};
}
#elif V == 2
// v2: removes the copies (string_view) and the stream, keeps std::map
[[gnu::noinline]] static std::vector<std::pair<std::string, long>> aggregate(const std::string& log) {
    std::map<std::string, long> total;
    std::string_view rest = log;
    while (!rest.empty()) {
        auto nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
        auto c1 = line.find(',');
        auto c3 = line.rfind(',');
        long bytes = 0;
        std::from_chars(line.data() + c3 + 1, line.data() + line.size(), bytes);
        total[std::string(line.substr(0, c1))] += bytes;
    }
    return {total.begin(), total.end()};
}
#else
// v3: hash map keyed by string_view into the log buffer (the log outlives the map)
[[gnu::noinline]] static std::vector<std::pair<std::string, long>> aggregate(const std::string& log) {
    std::unordered_map<std::string_view, long> total;
    total.reserve(8192);
    std::string_view rest = log;
    while (!rest.empty()) {
        auto nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
        auto c1 = line.find(',');
        auto c3 = line.rfind(',');
        long bytes = 0;
        std::from_chars(line.data() + c3 + 1, line.data() + line.size(), bytes);
        total[line.substr(0, c1)] += bytes;
    }
    std::vector<std::pair<std::string, long>> out;
    out.reserve(total.size());
    for (auto& [k, v] : total) out.emplace_back(std::string(k), v);
    return out;
}
#endif

int main(int argc, char** argv) {
    int lines = argc > 1 ? std::atoi(argv[1]) : 500000;
    std::string log = make_log(lines);
    auto t0 = std::chrono::steady_clock::now();
    auto agg = aggregate(log);
    std::sort(agg.begin(), agg.end(), [](auto& a, auto& b) { return a.second > b.second; });
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("v%d: %zu users, top=%s (%ld bytes), aggregate+sort %.0f ms\n", V, agg.size(), agg[0].first.c_str(),
                agg[0].second, ms);
}
