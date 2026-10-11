#pragma once
#include <memory>
#include <string>
class Widget {
public:
    explicit Widget(std::string pattern);
    ~Widget();
    Widget(Widget&&) noexcept;
    Widget& operator=(Widget&&) noexcept;
    bool matches(const std::string& s) const;
private:
    struct Impl;                  // incomplete here
    std::unique_ptr<Impl> impl_;
};
