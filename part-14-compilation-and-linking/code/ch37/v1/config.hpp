#pragma once
struct Config { int width; int height; };          // version 1
Config make_config();
int area(const Config&);
