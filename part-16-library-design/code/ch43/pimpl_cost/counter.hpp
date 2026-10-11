#pragma once
#include <memory>
class CounterPlain {                       // header-visible data + inline members
public:
    void add(int n) { total_ += n; }
    int  value() const { return total_; }
private:
    int total_ = 0;
};
class CounterPimpl {                       // data hidden; members defined in counter.cpp
public:
    CounterPimpl();
    ~CounterPimpl();
    void add(int n);
    int  value() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
