// g++ -std=c++20 -I../src math_test.cpp && ./a.out
#include "genie_math.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool near(double a, double b) {
    return std::abs(a - b) < 1e-6;
}

int main() {
    const genie::Pt T{960, 1031};
    for (double u : {0.0, 0.3, 1.0})
        for (double v : {0.0, 0.5, 1.0}) {
            // s = 0: the window untouched
            auto p = genie::point(200, 100, 800, 600, T, 42, 0, u, v);
            assert(near(p.x, 200 + u * 800) && near(p.y, 100 + v * 600));
            // s = 1: all of it inside the icon
            p = genie::point(200, 100, 800, 600, T, 42, 1, u, v);
            assert(near(p.y, T.y) && near(p.x, T.x + (u - 0.5) * 42));
            // a window reaching below the icon still ends in it
            p = genie::point(0, 0, 1920, 1080, T, 42, 1, u, v);
            assert(near(p.y, T.y));
            // and does not jump at the start
            p = genie::point(0, 0, 1920, 1080, T, 42, 0, u, v);
            assert(near(p.y, v * 1080));
        }
    // halfway, the bottom is narrower than the top
    auto tl = genie::point(200, 100, 800, 600, T, 42, 0.5, 0, 0), tr = genie::point(200, 100, 800, 600, T, 42, 0.5, 1, 0);
    auto bl = genie::point(200, 100, 800, 600, T, 42, 0.5, 0, 1), br = genie::point(200, 100, 800, 600, T, 42, 0.5, 1, 1);
    assert(br.x - bl.x < tr.x - tl.x);
    std::puts("genie_math ok");
}
