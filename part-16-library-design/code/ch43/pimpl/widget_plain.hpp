#pragma once
#include <regex>      // needed only because of a PRIVATE member
#include <string>
#include <vector>
class Widget {
public:
    explicit Widget(std::string pattern) : re_(std::move(pattern)) {}
    bool matches(const std::string& s) const { return std::regex_search(s, re_); }
private:
    std::regex re_;               // implementation detail, but now part of every includer's world
};
