struct Shape { virtual ~Shape() = default; virtual int area() const = 0; };
struct Sq final : Shape { int s; int area() const override { return s * s; } };
struct Rect : Shape       { int w, h; int area() const override { return w * h; } };

int via_base(const Shape& s)      { return s.area(); }                 // dynamic type unknown: must dispatch
int via_final(const Sq& s)        { return s.area(); }                 // Sq is final: no override possible
int via_rect(const Rect& r)       { return r.area(); }                 // Rect is not final: a subclass could override
int local_known() { Sq q; q.s = 7; const Shape& b = q; return b.area(); }   // dynamic type visible
