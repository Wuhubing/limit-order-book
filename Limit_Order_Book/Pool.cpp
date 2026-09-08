#include "Pool.hpp"
#include "Order.hpp"
#include "Limit.hpp"

// Explicit instantiation of the two concrete pool types used by the engine.
// The templated definitions live in Pool.hpp (they must be visible to Book's
// inline uses); this TU compiles the Order/Limit-heavy instantiations once so
// Pool.cpp is a real translation unit linked into the library.
template class Pool<Order, 4096>;
template class Pool<Limit, 4096>;
