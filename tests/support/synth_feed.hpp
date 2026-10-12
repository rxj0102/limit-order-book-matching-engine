#pragma once
// The synthetic feed generator now lives in the library (it is also exposed to Python);
// these aliases keep the tests unchanged.
#include "lobkit/sim/synthetic.hpp"

namespace lobkit::testing {
using sim::synthetic::make_synth_feed;
using sim::synthetic::SynthParams;
using sim::synthetic::VectorFeed;
}  // namespace lobkit::testing
