#include "audio_catalog.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <set>

int main() {
    using namespace audio_control;
    std::set<std::string> ids;
    for (const auto& parameter : Parameters) {
        assert(ids.insert(parameter.id).second);
        assert(parameter.minimum <= parameter.neutral && parameter.neutral <= parameter.maximum);
        assert(FindParameter(parameter.id) == &parameter);
    }
    assert(!FindParameter("voice_remover"));
    assert(!FindParameter("normalization"));
    assert(!FindParameter("replaygain"));
    assert(!FindParameter("PITCH"));
    assert(SafePresetName("My broadcast EQ"));
    assert(SafePresetName("Čisto — govor"));
    assert(!SafePresetName(""));
    assert(!SafePresetName("../existing"));
    assert(!SafePresetName("C:\\EQ"));
    assert(!SafePresetName("EQ\nnew"));
    assert(!SafePresetName(" trailing "));
    assert(!SafePresetName("ending."));
    assert(!SafePresetName(std::string(513, 'a')));
    const auto& pitch = *FindParameter("pitch");
    double value = 0;
    assert(NumberTarget({{"value", -10}}, false, 0, pitch.minimum, pitch.maximum, value) && value == -10);
    assert(!NumberTarget({{"value", -10.01}}, false, 0, pitch.minimum, pitch.maximum, value));
    assert(!NumberTarget({{"value", "1"}}, false, 0, -10, 10, value));
    assert(!NumberTarget({{"value", true}}, false, 0, -10, 10, value));
    assert(!NumberTarget({{"value", nullptr}}, false, 0, -10, 10, value));
    assert(!NumberTarget({}, false, 0, -10, 10, value));
    assert(!NumberTarget({{"value", std::numeric_limits<double>::infinity()}}, false, 0, -10, 10, value));
    assert(!NumberTarget({{"delta", std::numeric_limits<double>::quiet_NaN()}}, true, 0, -10, 10, value));
    assert(NumberTarget({{"delta", std::numeric_limits<double>::max()}}, true, 9.9, -10, 10, value) && value == 10);
    assert(NumberTarget({{"delta", -std::numeric_limits<double>::max()}}, true, -9.9, -10, 10, value) && value == -10);
    assert(!NumberTarget({{"delta", 1}}, true, std::numeric_limits<double>::infinity(), -10, 10, value));
    assert(NumberTarget({{"delta", -0.1}}, true, 1, -10, 10, value) && std::abs(value - 0.9) < 1e-12);
    assert(NumberTarget({{"delta", -5}}, true, 0.8, 0.8, 1.5, value) && value == 0.8);
    // AIMP may report enhancer 5 per the CHM although the C++ header caps at 4.
    // Clamp the result of the adjustment, rather than clamping before adding.
    assert(NumberTarget({{"delta", -0.05}}, true, 5, 1, 4, value) && value == 4);
    assert(NumberTarget({{"delta", 0.5}}, true, 14.8, EqMinimum, EqMaximum, value) && value == 15);
    assert(NumberTarget({{"value", -15}}, false, 0, EqMinimum, EqMaximum, value) && value == -15);
    int index = -1;
    assert(BandIndex({{"band", 0}}, 18, index) && index == 0);
    assert(BandIndex({{"band", 17}}, 18, index) && index == 17);
    assert(!BandIndex({{"band", 18}}, 18, index));
    assert(!BandIndex({{"band", -1}}, 18, index));
    assert(!BandIndex({{"band", 1.0}}, 18, index));
    assert(!BandIndex({{"band", "1"}}, 18, index));
    assert(!BandIndex({{"band", std::numeric_limits<std::uint64_t>::max()}}, 18, index));
    assert(!BandIndex({{"band", 0}}, 0, index));
    assert(!BandIndex({{"band", 0}}, -1, index));
    assert(!BandIndex({{"band", 0}}, MaxBands + 1, index));
    assert(DisplayNumber(0.10000000149011612) == 0.1);
    std::cout << "Audio range, finite-number, clamped-adjustment and dynamic-band validation passed\n";
}
