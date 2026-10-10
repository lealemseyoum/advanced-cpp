#include "widget.hpp"
struct Widget::Impl { double extra = 1.5; long more[4] = {}; int a = 42; };   // private layout changed freely
Widget::Widget() : impl_(new Impl) {}
Widget::~Widget() = default;
int Widget::value() const { return impl_->a; }
