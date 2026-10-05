#pragma once

#include <BufferManager.h>

enum class LossPolicy : uint8_t {
    EVICTED_OLDEST, EVICTED_PROTECTED, REJECTED_NEWEST, BUFFER_UNAVAILABLE
};
enum class JournalEventKind : uint8_t { LOSS, LEASE_ABANDONMENT };
enum class AbandonmentReason : uint8_t {
    BUFFER_PRESSURE_ACK_EXHAUSTED, BUFFER_PRESSURE_PUBLISH_FAILURE
};
enum class DeliveryOutcome : uint8_t { NOT_PUBLISHED, UNKNOWN_AFTER_PUBLISH };

struct LossRange {
    LossPolicy policy{};
    uint32_t boot_id = 0;
    uint32_t first_seq = 0;
    uint32_t last_seq = 0;
    uint32_t count = 0;
};

// Successful-publish count determines the outcome/reason; callers cannot label
// a published-but-unACKed record as confirmed lost. Production records this only
// after identity-checked, pressure-gated abandonment succeeds in BufferManager.
struct LeaseAbandonment {
    RecordIdentity identity{};
    uint32_t publish_calls = 0;
    uint32_t successful_publishes = 0;
    uint32_t connected_wait_ms = 0; // observed readiness diagnostic, not an abandonment budget
};

struct JournalEvent {
    JournalEventKind kind{};
    LossRange loss{}; // used only for LOSS
    LeaseAbandonment abandonment{}; // used only for LEASE_ABANDONMENT
    AbandonmentReason reason{};
    DeliveryOutcome delivery_outcome{};
};

struct JournalStats {
    size_t queued = 0;
    bool range_open = false;
    uint64_t buffer_unavailable = 0; // records, including those whose events were lost
    uint64_t diagnostic_events_lost = 0; // closed ranges/events, NOT telemetry records
    uint64_t lease_abandoned_unpublished = 0;
    uint64_t lease_abandoned_after_publish = 0;
};

// Bounded, RAM-only evidence. Runtime methods take one journal mutex and never
// call the buffer or MQTT. Forward PushOutcome AFTER push returns (no nested
// locks). Observation order can differ from sequence order: never sort or infer
// missing identities. begin/destruction require exclusive lifecycle access.
// Counters survive queue overflow, not reboot. No per-observation allocation.
class LossJournal {
public:
    static constexpr size_t kMaxEvents = 64;
    LossJournal() = default;
    ~LossJournal();
    LossJournal(const LossJournal&) = delete;
    LossJournal& operator=(const LossJournal&) = delete;

    // At most capacity closed events plus one open range. Range age is measured
    // from its FIRST observation, not extended by continued loss. Parameters
    // permit small deterministic tests; production uses these initial defaults.
    bool begin(size_t capacity = kMaxEvents, uint32_t maxSpan = 128,
               uint32_t intervalMs = 30000);
    // nowMs is monotonic millis, not UTC. Sample before calling. Small backwards
    // observations caused by mutex scheduling do not count as timer expiry.
    // True means accepted (possibly with counted diagnostic overflow), not durable.
    // False means uninitialised or malformed outcome. OK is an accepted no-op.
    bool observePush(const PushOutcome& outcome, uint32_t nowMs);
    bool observeAbandonment(const LeaseAbandonment& abandonment);
    void closeExpired(uint32_t nowMs); // connectionTask calls even without new loss
    void flush(); // explicitly close an open range; no network I/O
    // Sole consumer: connectionTask. pop transfers ownership; the later publisher
    // must retain its local event until sent. Never pop just to check availability.
    bool pop(JournalEvent& out);
    JournalStats getStats();

private:
    void closeRange(); // caller holds mutex
    void enqueue(const JournalEvent& event); // drop newest closed event if full
    bool expired(uint32_t nowMs) const;
    BufferPlatform::Mutex _mutex = nullptr;
    JournalEvent _events[kMaxEvents]{};
    JournalStats _stats{};
    LossRange _open{};
    size_t _capacity = 0;
    size_t _read = 0;
    uint32_t _maxSpan = 0;
    uint32_t _intervalMs = 0;
    uint32_t _openedMs = 0;
};
