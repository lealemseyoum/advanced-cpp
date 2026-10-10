#pragma once
struct Shape {
    virtual ~Shape();
    virtual int sides() const;
    virtual int corners() const;
};
Shape* make_shape();
