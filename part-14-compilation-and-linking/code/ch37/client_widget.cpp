#include "widget.hpp"
#include <cstdio>
int main() { Widget w; std::printf("client: Widget::value() = %d  (sizeof(Widget) = %zu)\n", w.value(), sizeof(Widget)); }
