#ifndef R2_MEMENTO_COUNTER_HPP
#define R2_MEMENTO_COUNTER_HPP
#include "../../host.hpp"

constexpr uint32_t COUNTER_MAGIC = 0x54433252;
constexpr uint32_t COUNTER_VERSION = 1;
enum CounterOp { CounterIncrement = 1 };
struct CounterSnapshot { uint32_t count; char label[64]; };
using CounterBlock = r2memento::Block<CounterSnapshot>;
using CounterHost = r2memento::Host<CounterSnapshot>;
static_assert(sizeof(CounterSnapshot) == 68, "Counter snapshot ABI");
static_assert(sizeof(CounterBlock) == 568, "Counter block ABI");
#endif
