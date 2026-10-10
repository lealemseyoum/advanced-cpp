#include "config.hpp"
Config make_config() { Config c{}; c.width = 3; c.height = 4; return c; }
int area(const Config& c) { return c.width * c.height; }
