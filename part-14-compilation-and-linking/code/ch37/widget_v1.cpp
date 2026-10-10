#include "widget.hpp"
struct Widget::Impl { int a = 10; };
Widget::Widget() : impl_(new Impl) {}
Widget::~Widget() = default;
int Widget::value() const { return impl_->a; }
