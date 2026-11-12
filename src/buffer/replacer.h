#pragma once

#include "common/config.h"

namespace arbordb {

// Strategy pattern: the buffer pool depends only on this interface, so the eviction policy is swappable.
class Replacer {
public:
    virtual ~Replacer() = default;
    // Pick an evictable frame and forget it. Returns false when nothing is evictable.
    virtual bool Victim(frame_id_t* out) = 0;
    // Frame is in use: it must not be returned by Victim().
    virtual void Pin(frame_id_t f) = 0;
    // Frame is no longer in use: it becomes a Victim() candidate.
    virtual void Unpin(frame_id_t f) = 0;
    // Note that the frame was accessed (feeds the policy's history).
    virtual void RecordAccess(frame_id_t f) = 0;
    // Drop all state for a frame (page deleted).
    virtual void Remove(frame_id_t f) = 0;
    virtual std::size_t Size() const = 0;  // number of evictable frames
};

}  // namespace arbordb
