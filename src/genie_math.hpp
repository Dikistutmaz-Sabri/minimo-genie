#pragma once
// Genie geometry, no Hyprland types: a point of the window (u, v in 0..1) at
// progress s (0 = the window as it is, 1 = gone into the icon).
// Coordinates are monitor-local pixels.
// shortcut: assumes the dock is below the window (Minimo's dock is at the bottom); a dock on a side needs the axes swapped.

#include <algorithm>
#include <cmath>

namespace genie {
    struct Pt {
        double x = 0, y = 0;
    };

    inline double smoothstep(double a, double b, double x) {
        if (b <= a)
            return x >= b ? 1.0 : 0.0;
        const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    inline double lerp(double a, double b, double t) {
        return a + (b - a) * t;
    }

    // win: x, y, w, h. target: the icon's centre. iconW: the width the window narrows to.
    inline Pt point(double wx, double wy, double ww, double wh, Pt target, double iconW, double s, double u, double v) {
        const double bend  = smoothstep(0.0, 0.4, s);  // first the sides bend towards the icon
        const double slide = smoothstep(0.2, 1.0, s);  // then the window flows down the funnel
        const double yRest = wy + v * wh;
        const double H     = std::max(target.y - wy, 1.0);

        // rows go down the funnel and pile up at the icon; rows already below it wait, then are pulled in at the end
        double y = std::min(yRest + slide * H, std::max(target.y, yRest));
        y        = lerp(y, target.y, smoothstep(0.7, 1.0, s));

        // the funnel: full width at the window's top, the icon's width at the icon
        const double f  = target.y > wy ? smoothstep(wy, target.y, y) : 1.0;
        const double hw = lerp(ww / 2.0, iconW / 2.0, f);
        const double c  = lerp(wx + ww / 2.0, target.x, f);
        const double xg = c + (u - 0.5) * 2.0 * hw;

        return {lerp(wx + u * ww, xg, bend), y};
    }
}
