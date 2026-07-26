#pragma once

#include <cstddef>
#include <cstdint>

namespace Exchange::Net {
/*
The constants that more than one place has to agree on.

That is the entire admission rule, and it is worth stating because the obvious
alternative — sweeping every tunable in the layer into one file — would make
things worse. Most of the numbers in this layer are single-use policy knobs
that live next to a comment explaining why they are what they are:
MatchingThread::kBatchCap (the round-robin fairness knob),
SessionPump::kMaxInFlight (per-session credit), EgressQueue::kDrainBatch,
IoThread::kResumeFreeSlots (the ingress low-water mark). Moving those here
would separate each number from its reasoning and leave a file of bare
integers nobody can review. They stay where they are on purpose.

What belongs here is the opposite case: a value that is written in several
places and must match in all of them, where a single-site edit is a silent
bug.
*/

/*
Default L2 depth published per book.

Written in three places before this existed — MatchingLoopConfig's default,
BookInfo's NSDMI, and MarketDataPublisher::BookMd's. Depth is a property of
the BOOK rather than of the subscriber (md_seq is per book, so subscribers at
different depths would see gaps in a sequence that has none), which means a
book registered through a path that did not carry the configured depth would
silently publish a different stream. One definition removes the possibility.
*/
inline constexpr uint32_t kDefaultMdDepth{10};

// Wire prices are integers; the display value is price / 10^price_scale. No
// floats ever go on the wire, in either protocol.
inline constexpr uint32_t kDefaultPriceScale{2};

/*
Ring sizes, kept together so the benchmark can sweep them from one place.

Ingress is smaller than egress on purpose: one command can produce many events
(a snapshot is 1 + N + 1 of them), so the egress side needs the bigger
cushion.

These live here rather than in SpscRing.hpp because they are the
application's sizes, not the ring's — the template is a general-purpose queue
and has no business knowing how deep this exchange's pipelines are.
*/
inline constexpr std::size_t kIngressRingCapacity{2048};
inline constexpr std::size_t kEgressRingCapacity{4096};
}  // namespace Exchange::Net
