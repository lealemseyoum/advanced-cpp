#include "widget_pimpl.hpp"
#include <regex>
struct Widget::Impl { std::regex re; };
Widget::Widget(std::string p) : impl_(std::make_unique<Impl>(Impl{std::regex(std::move(p))})) {}
Widget::~Widget() = default;
Widget::Widget(Widget&&) noexcept = default;
Widget& Widget::operator=(Widget&&) noexcept = default;
bool Widget::matches(const std::string& s) const { return std::regex_search(s, impl_->re); }
