#pragma once
struct Shape {
    virtual ~Shape();
    virtual const char* name() const;      // v2: new virtual inserted BEFORE the existing ones
    virtual int sides() const;
    virtual int corners() const;
};
Shape* make_shape();
