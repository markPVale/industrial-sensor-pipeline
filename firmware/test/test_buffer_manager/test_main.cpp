#include <unity.h>
#include <BufferManager.h>
#include <atomic>
#include <deque>
#include <thread>
#include <vector>

static BufferManager buffer;

static TelemetryRecord record(uint32_t seq, uint32_t boot = 7) {
    TelemetryRecord r{};
    r.boot_id = boot;
    r.sequence_id = seq;
    r.timestamp_ms = 1700000000000ULL + seq;
    r.accel_x = 1.25f; r.accel_y = -2.5f; r.accel_z = 9.81f;
    r.gyro_x = 3.5f; r.gyro_y = -4.5f; r.gyro_z = 5.5f;
    r.window_rms = 10.5f;
    r.status_flags = STATUS_INTERLOCK_OPEN;
    return r;
}

static void assertIdentity(const RecordIdentity& id, const TelemetryRecord& r) {
    TEST_ASSERT_EQUAL_UINT32(r.boot_id, id.boot_id);
    TEST_ASSERT_EQUAL_UINT32(r.sequence_id, id.sequence_id);
}

static void assertRecord(const TelemetryRecord& expected, const TelemetryRecord& actual) {
    assertIdentity({actual.boot_id, actual.sequence_id}, expected);
    TEST_ASSERT_EQUAL_UINT64(expected.timestamp_ms, actual.timestamp_ms);
    TEST_ASSERT_EQUAL_FLOAT(expected.accel_x, actual.accel_x);
    TEST_ASSERT_EQUAL_FLOAT(expected.accel_y, actual.accel_y);
    TEST_ASSERT_EQUAL_FLOAT(expected.accel_z, actual.accel_z);
    TEST_ASSERT_EQUAL_FLOAT(expected.gyro_x, actual.gyro_x);
    TEST_ASSERT_EQUAL_FLOAT(expected.gyro_y, actual.gyro_y);
    TEST_ASSERT_EQUAL_FLOAT(expected.gyro_z, actual.gyro_z);
    TEST_ASSERT_EQUAL_FLOAT(expected.window_rms, actual.window_rms);
    TEST_ASSERT_EQUAL_UINT8(expected.status_flags, actual.status_flags);
}

void setUp() {}
void tearDown() {
    TelemetryRecord oldest{};
    if (buffer.peek(oldest)) buffer.commitPinnedHead(oldest.boot_id, oldest.sequence_id);
    buffer.end();
}

static void test_lifecycle_and_uninitialised_operations() {
    TelemetryRecord out = record(999);
    RecordIdentity lost{99, 99};
    TEST_ASSERT_FALSE(buffer.begin(0));
    TEST_ASSERT_FALSE(buffer.begin(SIZE_MAX / sizeof(TelemetryRecord) + 1));
    const auto result = buffer.push(record(1));
    TEST_ASSERT_EQUAL_INT((int)PushResult::NOT_INITIALISED, (int)result.result);
    TEST_ASSERT_TRUE(result.record_lost);
    assertIdentity(result.lost, record(1));
    TEST_ASSERT_FALSE(buffer.peek(out));
    TEST_ASSERT_FALSE(buffer.pop(out));
    TEST_ASSERT_FALSE(buffer.acquirePin(out));
    TEST_ASSERT_FALSE(buffer.commitPinnedHead(7, 1));
    TEST_ASSERT_FALSE(buffer.abandonPinnedHead(7, 1, lost));
    TEST_ASSERT_EQUAL_UINT32(99, lost.sequence_id);
    assertRecord(record(999), out);
    TEST_ASSERT_FALSE(buffer.clear());
    TEST_ASSERT_TRUE(buffer.isEmpty());
    TEST_ASSERT_FALSE(buffer.isFull());
    TEST_ASSERT_EQUAL_UINT(0, buffer.capacity());
    TEST_ASSERT_TRUE(buffer.end());
    TEST_ASSERT_TRUE(buffer.begin(2));
    TEST_ASSERT_FALSE(buffer.begin(3));
    TEST_ASSERT_FALSE(buffer.acquirePin(out));
    TEST_ASSERT_TRUE(buffer.end());
    TEST_ASSERT_TRUE(buffer.begin(1));
}

static void test_unpinned_fifo_and_exact_overflow_identity() {
    TEST_ASSERT_TRUE(buffer.begin(2));
    auto result = buffer.push(record(80, 9));
    TEST_ASSERT_FALSE(result.record_lost);
    buffer.push(record(2, 10)); // physical order, not sequence or boot sorting
    result = buffer.push(record(1, 3));
    TEST_ASSERT_EQUAL_INT((int)PushResult::EVICTED_OLDEST, (int)result.result);
    assertIdentity(result.lost, record(80, 9));
    TelemetryRecord out{};
    TEST_ASSERT_TRUE(buffer.pop(out)); assertRecord(record(2, 10), out);
    TEST_ASSERT_TRUE(buffer.pop(out)); assertRecord(record(1, 3), out);
    TEST_ASSERT_FALSE(buffer.pop(out));
    TEST_ASSERT_EQUAL_UINT(1, buffer.getStats().evicted_oldest);
    TEST_ASSERT_EQUAL_UINT(1, buffer.droppedCount());
    TEST_ASSERT_EQUAL_UINT(2, buffer.freeSpace());
    TEST_ASSERT_TRUE(buffer.clear());
    TEST_ASSERT_EQUAL_UINT(0, buffer.droppedCount());
}

static void test_pin_survives_fill_relocation_and_wraparound() {
    TEST_ASSERT_TRUE(buffer.begin(3));
    buffer.push(record(100));
    TelemetryRecord out{};
    TEST_ASSERT_TRUE(buffer.acquirePin(out));
    buffer.push(record(1)); buffer.push(record(2));
    for (uint32_t seq = 3; seq < 40; ++seq) {
        const auto result = buffer.push(record(seq));
        TEST_ASSERT_EQUAL_INT((int)PushResult::EVICTED_PROTECTED, (int)result.result);
        assertIdentity(result.lost, record(seq - 2));
        TEST_ASSERT_TRUE(buffer.peek(out)); assertRecord(record(100), out);
        TEST_ASSERT_EQUAL_UINT(3, buffer.available());
    }
    TEST_ASSERT_EQUAL_UINT(37, buffer.getStats().protected_evictions);
    TEST_ASSERT_TRUE(buffer.commitPinnedHead(7, 100));
    TEST_ASSERT_TRUE(buffer.pop(out)); assertRecord(record(38), out);
    TEST_ASSERT_TRUE(buffer.pop(out)); assertRecord(record(39), out);
    TEST_ASSERT_TRUE(buffer.isEmpty());
}

static void test_capacity_one_protects_pin_and_counts_rejection() {
    TEST_ASSERT_TRUE(buffer.begin(1));
    buffer.push(record(0));
    auto result = buffer.push(record(1));
    TEST_ASSERT_EQUAL_INT((int)PushResult::EVICTED_OLDEST, (int)result.result);
    TelemetryRecord out{};
    TEST_ASSERT_TRUE(buffer.acquirePin(out));
    result = buffer.push(record(2));
    TEST_ASSERT_EQUAL_INT((int)PushResult::REJECTED_NEWEST, (int)result.result);
    assertIdentity(result.lost, record(2));
    TEST_ASSERT_EQUAL_UINT(1, buffer.getStats().rejected_newest);
    TEST_ASSERT_EQUAL_UINT(1, buffer.droppedCount()); // rejection is not eviction
    TEST_ASSERT_FALSE(buffer.pop(out));
    TEST_ASSERT_FALSE(buffer.clear());
    TEST_ASSERT_FALSE(buffer.end());
    out = record(999);
    TEST_ASSERT_FALSE(buffer.acquirePin(out));
    assertRecord(record(999), out);
    TEST_ASSERT_TRUE(buffer.commitPinnedHead(7, 1));
    TEST_ASSERT_FALSE(buffer.push(record(3)).record_lost);
}

static void test_identity_checks_and_counted_abandonment() {
    TEST_ASSERT_TRUE(buffer.begin(2));
    buffer.push(record(1)); buffer.push(record(2));
    TEST_ASSERT_FALSE(buffer.commitPinnedHead(7, 1)); // identity alone is insufficient
    TelemetryRecord out{};
    TEST_ASSERT_TRUE(buffer.acquirePin(out));
    TEST_ASSERT_FALSE(buffer.commitPinnedHead(8, 1));
    TEST_ASSERT_FALSE(buffer.commitPinnedHead(7, 2));
    RecordIdentity abandoned{99, 99};
    TEST_ASSERT_FALSE(buffer.abandonPinnedHead(8, 1, abandoned));
    TEST_ASSERT_FALSE(buffer.abandonPinnedHead(7, 2, abandoned));
    TEST_ASSERT_EQUAL_UINT32(99, abandoned.boot_id);
    TEST_ASSERT_EQUAL_UINT32(99, abandoned.sequence_id);
    TEST_ASSERT_EQUAL_UINT(2, buffer.available());
    TEST_ASSERT_EQUAL_UINT(0, buffer.getStats().abandoned);
    TEST_ASSERT_TRUE(buffer.commitPinnedHead(7, 1));
    TEST_ASSERT_TRUE(buffer.acquirePin(out));
    TEST_ASSERT_FALSE(buffer.commitPinnedHead(7, 1)); // duplicate ACK cannot remove R1
    TEST_ASSERT_TRUE(buffer.peek(out)); assertRecord(record(2), out);
    TEST_ASSERT_TRUE(buffer.abandonPinnedHead(7, 2, abandoned));
    assertIdentity(abandoned, record(2));
    TEST_ASSERT_FALSE(buffer.abandonPinnedHead(7, 2, abandoned));
    TEST_ASSERT_EQUAL_UINT(1, buffer.getStats().abandoned);
    TEST_ASSERT_EQUAL_UINT(0, buffer.droppedCount());
    TEST_ASSERT_TRUE(buffer.clear());
    TEST_ASSERT_EQUAL_UINT(0, buffer.getStats().abandoned);
}

static void test_ring_matches_deque_model() {
    uint32_t rng = 0x5eed;
    for (size_t capacity = 1; capacity <= 7; ++capacity) {
        TEST_ASSERT_TRUE(buffer.begin(capacity));
        std::deque<TelemetryRecord> model;
        bool pinned = false;
        size_t evicted = 0, protectedEvictions = 0, rejected = 0, abandoned = 0;
        for (uint32_t step = 0; step < 2000; ++step) {
            rng = rng * 1664525U + 1013904223U;
            const unsigned action = (rng >> 16) % 8;
            TelemetryRecord out = record(99999);
            if (action < 4) {
                auto incoming = record(step, (step % 3) + 1);
                auto expected = PushResult::OK;
                RecordIdentity victim{};
                if (model.size() == capacity) {
                    const auto lost = pinned && capacity == 1 ? incoming : model[pinned ? 1 : 0];
                    victim = {lost.boot_id, lost.sequence_id};
                    if (pinned && capacity == 1) { expected = PushResult::REJECTED_NEWEST; ++rejected; }
                    else {
                        expected = pinned ? PushResult::EVICTED_PROTECTED : PushResult::EVICTED_OLDEST;
                        if (pinned) ++protectedEvictions; else ++evicted;
                        model.erase(model.begin() + (pinned ? 1 : 0));
                    }
                }
                if (expected != PushResult::REJECTED_NEWEST) model.push_back(incoming);
                const auto actual = buffer.push(incoming);
                TEST_ASSERT_EQUAL_INT((int)expected, (int)actual.result);
                TEST_ASSERT_EQUAL(expected != PushResult::OK, actual.record_lost);
                if (actual.record_lost) TEST_ASSERT_TRUE(victim == actual.lost);
            } else if (action == 4) {
                const bool canAcquire = !pinned && !model.empty();
                TEST_ASSERT_EQUAL(canAcquire, buffer.acquirePin(out));
                if (canAcquire) { pinned = true; assertRecord(model.front(), out); }
                else assertRecord(record(99999), out);
            } else if (action == 5 || action == 6) {
                const auto first = model.empty() ? record(99999) : model.front();
                RecordIdentity lost{};
                const bool removed = action == 5
                    ? buffer.commitPinnedHead(first.boot_id, first.sequence_id)
                    : buffer.abandonPinnedHead(first.boot_id, first.sequence_id, lost);
                TEST_ASSERT_EQUAL(pinned, removed);
                if (removed) {
                    if (action == 6) { ++abandoned; assertIdentity(lost, first); }
                    model.pop_front(); pinned = false;
                }
            } else {
                const bool canPop = !pinned && !model.empty();
                TEST_ASSERT_EQUAL(canPop, buffer.pop(out));
                if (canPop) { assertRecord(model.front(), out); model.pop_front(); }
            }
            const auto stats = buffer.getStats();
            TEST_ASSERT_EQUAL_UINT(model.size(), stats.count);
            TEST_ASSERT_EQUAL_UINT(capacity - model.size(), stats.free_space);
            TEST_ASSERT_EQUAL_UINT(evicted, stats.evicted_oldest);
            TEST_ASSERT_EQUAL_UINT(protectedEvictions, stats.protected_evictions);
            TEST_ASSERT_EQUAL_UINT(rejected, stats.rejected_newest);
            TEST_ASSERT_EQUAL_UINT(abandoned, stats.abandoned);
            TEST_ASSERT_EQUAL_UINT(evicted + protectedEvictions, stats.dropped);
            TEST_ASSERT_EQUAL(!model.empty(), buffer.peek(out));
            if (!model.empty()) assertRecord(model.front(), out);
        }
        if (pinned) {
            const auto first = model.front();
            TEST_ASSERT_TRUE(buffer.commitPinnedHead(first.boot_id, first.sequence_id));
            model.pop_front();
        }
        while (!model.empty()) {
            TelemetryRecord out{};
            TEST_ASSERT_TRUE(buffer.pop(out)); assertRecord(model.front(), out);
            model.pop_front();
        }
        TEST_ASSERT_TRUE(buffer.end());
    }
}

static void test_two_producers_and_pinned_consumer_account_for_every_record() {
    TEST_ASSERT_TRUE(buffer.begin(4));
    constexpr uint32_t perProducer = 2000;
    std::atomic<unsigned> finished{0};
    std::atomic<bool> start{false};
    std::vector<RecordIdentity> losses[2];
    std::vector<RecordIdentity> commits;
    auto produce = [&](unsigned producer) {
        while (!start.load()) std::this_thread::yield();
        for (uint32_t seq = 0; seq < perProducer; ++seq) {
            const auto result = buffer.push(record(seq, producer + 1));
            if (result.record_lost) losses[producer].push_back(result.lost);
            std::this_thread::yield();
        }
        ++finished;
    };
    std::thread a(produce, 0), b(produce, 1);
    start = true;
    bool valid = true;
    // Bounded consumer loop: join before asserting, including on failure.
    for (unsigned attempt = 0; attempt < 50000; ++attempt) {
        TelemetryRecord pinned{}, oldest{};
        if (buffer.acquirePin(pinned)) {
            std::this_thread::yield();
            valid = buffer.peek(oldest) && oldest.boot_id == pinned.boot_id &&
                    oldest.sequence_id == pinned.sequence_id && valid;
            if (buffer.commitPinnedHead(pinned.boot_id, pinned.sequence_id))
                commits.push_back({pinned.boot_id, pinned.sequence_id});
            else { valid = false; break; }
        }
        if (finished == 2 && buffer.isEmpty()) break;
        std::this_thread::yield();
    }
    a.join(); b.join();
    TelemetryRecord remaining{};
    while (buffer.acquirePin(remaining)) {
        if (!buffer.commitPinnedHead(remaining.boot_id, remaining.sequence_id)) { valid = false; break; }
        commits.push_back({remaining.boot_id, remaining.sequence_id});
    }
    TEST_ASSERT_TRUE(valid);
    TEST_ASSERT_TRUE(buffer.isEmpty());
    std::vector<unsigned> terminalCount(perProducer * 2, 0);
    auto count = [&](RecordIdentity id) {
        TEST_ASSERT_TRUE(id.boot_id == 1 || id.boot_id == 2);
        TEST_ASSERT_LESS_THAN_UINT32(perProducer, id.sequence_id);
        ++terminalCount[(id.boot_id - 1) * perProducer + id.sequence_id];
    };
    for (auto id : commits) count(id);
    for (auto& producerLosses : losses) for (auto id : producerLosses) count(id);
    for (auto n : terminalCount) TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT(losses[0].size() + losses[1].size(), buffer.droppedCount());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_lifecycle_and_uninitialised_operations);
    RUN_TEST(test_unpinned_fifo_and_exact_overflow_identity);
    RUN_TEST(test_pin_survives_fill_relocation_and_wraparound);
    RUN_TEST(test_capacity_one_protects_pin_and_counts_rejection);
    RUN_TEST(test_identity_checks_and_counted_abandonment);
    RUN_TEST(test_ring_matches_deque_model);
    RUN_TEST(test_two_producers_and_pinned_consumer_account_for_every_record);
    return UNITY_END();
}
