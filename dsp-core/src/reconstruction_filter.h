#ifndef DREAMGUARD_RECONSTRUCTION_FILTER_H
#define DREAMGUARD_RECONSTRUCTION_FILTER_H

#include <cmath>

namespace dreamguard {
constexpr int kPhases = 4;
constexpr int kHistory = 17;

// Shared by the measurement and the conservative limiting bound. Constructed
// only at handle creation; processing does not evaluate trig functions.
template <int History>
inline void build_reconstruction_filter(double (&coefficients)[kPhases][History]) {
    constexpr double pi = 3.14159265358979323846;
    constexpr int last = (History - 1) * kPhases;
    constexpr int center = last / 2;
    for (int phase = 0; phase < kPhases; ++phase) {
        double sum = 0.0;
        for (int tap = 0; tap < History; ++tap) {
            const int j = phase + kPhases * tap;
            const double x = (j - center) / 4.0;
            const double sinc = j == center ? 1.0 : std::sin(pi * x) / (pi * x);
            const double angle = 2.0 * pi * j / last;
            const double window = 0.42 - 0.5 * std::cos(angle) + 0.08 * std::cos(2.0 * angle);
            coefficients[phase][tap] = j <= last ? sinc * window : 0.0;
            sum += coefficients[phase][tap];
        }
        for (int tap = 0; tap < History; ++tap) {
            coefficients[phase][tap] =
                phase == 0 ? (tap == center / kPhases ? 1.0 : 0.0) : coefficients[phase][tap] / sum;
        }
    }
}
}  // namespace dreamguard
#endif
