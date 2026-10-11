#include "counter.hpp"
struct CounterPimpl::Impl { int total = 0; };
CounterPimpl::CounterPimpl() : impl_(std::make_unique<Impl>()) {}
CounterPimpl::~CounterPimpl() = default;
void CounterPimpl::add(int n) { impl_->total += n; }
int  CounterPimpl::value() const { return impl_->total; }
