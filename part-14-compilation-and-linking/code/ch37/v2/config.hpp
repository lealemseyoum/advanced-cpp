#pragma once
struct Config { int width; int depth; int height; };   // version 2: a field was inserted in the middle
Config make_config();
int area(const Config&);
