#include <unity.h>
#include <LossJournal.h>
#include <atomic>
#include <thread>
#include <vector>

void setUp() {}
void tearDown() {}

static PushOutcome loss(uint32_t seq, PushResult policy = PushResult::EVICTED_OLDEST,
                        uint32_t boot = 7) {
    return {policy, true, {boot, seq}};
}

static void expectRange(LossJournal& journal, uint32_t first, uint32_t last,
                        uint32_t count, LossPolicy policy = LossPolicy::EVICTED_OLDEST,
                        uint32_t boot = 7) {
    JournalEvent event{};
    TEST_ASSERT_TRUE(journal.pop(event));
    TEST_ASSERT_EQUAL_INT((int)JournalEventKind::LOSS, (int)event.kind);
    TEST_ASSERT_EQUAL_INT((int)policy, (int)event.loss.policy);
    TEST_ASSERT_EQUAL_UINT32(boot, event.loss.boot_id);
    TEST_ASSERT_EQUAL_UINT32(first, event.loss.first_seq);
    TEST_ASSERT_EQUAL_UINT32(last, event.loss.last_seq);
    TEST_ASSERT_EQUAL_UINT32(count, event.loss.count);
}

static void test_lifecycle_validation_and_ok_noop() {
    LossJournal journal;
    TEST_ASSERT_FALSE(journal.observePush(loss(1), 0));
    TEST_ASSERT_FALSE(journal.observeAbandonment({}));
    JournalEvent event{}; event.loss.first_seq = 99;
    TEST_ASSERT_FALSE(journal.pop(event));
    TEST_ASSERT_EQUAL_UINT32(99, event.loss.first_seq);
    journal.flush(); journal.closeExpired(100);
    TEST_ASSERT_FALSE(journal.begin(0));
    TEST_ASSERT_FALSE(journal.begin(LossJournal::kMaxEvents + 1));
    TEST_ASSERT_FALSE(journal.begin(2, 0));
    TEST_ASSERT_FALSE(journal.begin(2, 3, 0));
    TEST_ASSERT_FALSE(journal.begin(2, 3, 0x80000000U));
    TEST_ASSERT_TRUE(journal.begin());
    TEST_ASSERT_FALSE(journal.begin());
    TEST_ASSERT_TRUE(journal.observePush({PushResult::OK, false, {}}, 0));
    TEST_ASSERT_FALSE(journal.observePush({PushResult::OK, true, {}}, 0));
    TEST_ASSERT_FALSE(journal.observePush({PushResult::REJECTED_NEWEST, false, {}}, 0));
    TEST_ASSERT_FALSE(journal.observePush({static_cast<PushResult>(255), true, {}}, 0));
    LeaseAbandonment invalid{}; invalid.successful_publishes = 1;
    TEST_ASSERT_FALSE(journal.observeAbandonment(invalid));
    TEST_ASSERT_EQUAL_UINT(0, journal.getStats().queued);
    TEST_ASSERT_FALSE(journal.getStats().range_open);
}

static void test_adjacent_only_compression_in_observation_order() {
    LossJournal journal; TEST_ASSERT_TRUE(journal.begin());
    journal.observePush(loss(100), 0);
    journal.observePush(loss(101), 1);
    journal.observePush(loss(150), 2); // do not claim delivered 102..149
    journal.observePush(loss(149), 3); // out-of-order producer
    journal.observePush(loss(149), 4); // duplicate observations are not adjacency
    journal.observePush(loss(150, PushResult::EVICTED_OLDEST, 8), 5);
    journal.observePush(loss(151, PushResult::EVICTED_PROTECTED, 8), 6);
    journal.flush();
    expectRange(journal, 100, 101, 2);
    expectRange(journal, 150, 150, 1);
    expectRange(journal, 149, 149, 1);
    expectRange(journal, 149, 149, 1);
    expectRange(journal, 150, 150, 1, LossPolicy::EVICTED_OLDEST, 8);
    expectRange(journal, 151, 151, 1, LossPolicy::EVICTED_PROTECTED, 8);
    TEST_ASSERT_EQUAL_UINT(0, journal.getStats().queued);
}

static void test_span_and_timer_close_without_another_loss() {
    LossJournal journal; TEST_ASSERT_TRUE(journal.begin(8, 2, 100));
    journal.observePush(loss(1), 0); journal.observePush(loss(2), 1);
    TEST_ASSERT_FALSE(journal.getStats().range_open);
    expectRange(journal, 1, 2, 2);
    journal.observePush(loss(3), 20);
    journal.closeExpired(119); TEST_ASSERT_EQUAL_UINT(0, journal.getStats().queued);
    journal.closeExpired(120); expectRange(journal, 3, 3, 1);
    journal.closeExpired(121); journal.flush();
    TEST_ASSERT_EQUAL_UINT(0, journal.getStats().queued);
    journal.observePush(loss(4), 200);
    journal.observePush(loss(5), 300); // expiry checked before compression
    expectRange(journal, 4, 4, 1);
    journal.flush(); expectRange(journal, 5, 5, 1);
}

static void test_timer_not_extended_and_clock_wrap() {
    LossJournal journal; TEST_ASSERT_TRUE(journal.begin(8, 128, 100));
    journal.observePush(loss(1), 1000);
    journal.observePush(loss(2), 1090);
    journal.closeExpired(1100); expectRange(journal, 1, 2, 2);
    journal.observePush(loss(UINT32_MAX), UINT32_MAX - 49);
    journal.observePush(loss(0), UINT32_MAX - 50); // older timestamp, same mutex order
    TEST_ASSERT_EQUAL_UINT(0, journal.getStats().queued);
    journal.closeExpired(49); TEST_ASSERT_EQUAL_UINT(0, journal.getStats().queued);
    journal.closeExpired(50);
    expectRange(journal, UINT32_MAX, 0, 2); // explicit uint32 sequence-wrap policy
}

static void test_overflow_counts_events_not_records_and_queue_wraps() {
    LossJournal journal; TEST_ASSERT_TRUE(journal.begin(1, 2, 100));
    journal.observePush(loss(1, PushResult::NOT_INITIALISED), 0);
    journal.observePush(loss(2, PushResult::NOT_INITIALISED), 1);
    journal.observePush(loss(3, PushResult::NOT_INITIALISED), 2);
    journal.observePush(loss(4, PushResult::NOT_INITIALISED), 3);
    auto stats = journal.getStats();
    TEST_ASSERT_EQUAL_UINT64(4, stats.buffer_unavailable);
    TEST_ASSERT_EQUAL_UINT64(1, stats.diagnostic_events_lost); // one two-record range
    expectRange(journal, 1, 2, 2, LossPolicy::BUFFER_UNAVAILABLE);
    for (uint32_t seq = 5; seq < 15; ++seq) {
        journal.observePush(loss(seq, PushResult::REJECTED_NEWEST), seq);
        journal.flush();
        expectRange(journal, seq, seq, 1, LossPolicy::REJECTED_NEWEST);
    }
    TEST_ASSERT_EQUAL_UINT64(1, journal.getStats().diagnostic_events_lost);
}

static void test_abandonment_is_outcome_not_confirmed_loss() {
    LossJournal journal; TEST_ASSERT_TRUE(journal.begin());
    journal.observePush(loss(1), 0);
    LeaseAbandonment abandoned{};
    abandoned.identity = {7, 2}; abandoned.publish_calls = 5;
    abandoned.successful_publishes = 2; abandoned.connected_wait_ms = 15000;
    TEST_ASSERT_TRUE(journal.observeAbandonment(abandoned));
    abandoned.identity.sequence_id = 3; abandoned.successful_publishes = 0;
    TEST_ASSERT_TRUE(journal.observeAbandonment(abandoned));
    expectRange(journal, 1, 1, 1);
    JournalEvent event{};
    TEST_ASSERT_TRUE(journal.pop(event));
    TEST_ASSERT_EQUAL_INT((int)JournalEventKind::LEASE_ABANDONMENT, (int)event.kind);
    TEST_ASSERT_EQUAL_INT((int)DeliveryOutcome::UNKNOWN_AFTER_PUBLISH, (int)event.delivery_outcome);
    TEST_ASSERT_EQUAL_INT((int)AbandonmentReason::BUFFER_PRESSURE_ACK_EXHAUSTED, (int)event.reason);
    TEST_ASSERT_EQUAL_UINT32(7, event.abandonment.identity.boot_id);
    TEST_ASSERT_EQUAL_UINT32(2, event.abandonment.identity.sequence_id);
    TEST_ASSERT_EQUAL_UINT32(5, event.abandonment.publish_calls);
    TEST_ASSERT_EQUAL_UINT32(2, event.abandonment.successful_publishes);
    TEST_ASSERT_EQUAL_UINT32(15000, event.abandonment.connected_wait_ms);
    TEST_ASSERT_TRUE(journal.pop(event));
    TEST_ASSERT_EQUAL_INT((int)DeliveryOutcome::NOT_PUBLISHED, (int)event.delivery_outcome);
    TEST_ASSERT_EQUAL_INT((int)AbandonmentReason::BUFFER_PRESSURE_PUBLISH_FAILURE, (int)event.reason);
    TEST_ASSERT_EQUAL_UINT64(1, journal.getStats().lease_abandoned_after_publish);
    TEST_ASSERT_EQUAL_UINT64(1, journal.getStats().lease_abandoned_unpublished);
}

static void test_abandonment_counters_survive_journal_overflow() {
    LossJournal journal; TEST_ASSERT_TRUE(journal.begin(1, 1));
    journal.observePush(loss(0), 0);
    LeaseAbandonment a{}; a.identity = {7, 1}; a.publish_calls = 5;
    journal.observeAbandonment(a);
    a.identity.sequence_id = 2; a.successful_publishes = 1;
    journal.observeAbandonment(a);
    const auto stats = journal.getStats();
    TEST_ASSERT_EQUAL_UINT64(2, stats.diagnostic_events_lost);
    TEST_ASSERT_EQUAL_UINT64(1, stats.lease_abandoned_unpublished);
    TEST_ASSERT_EQUAL_UINT64(1, stats.lease_abandoned_after_publish);
    expectRange(journal, 0, 0, 1);
}

static void test_real_buffer_outcomes_forward_exact_victims() {
    BufferManager buffer;
    LossJournal journal; TEST_ASSERT_TRUE(journal.begin());
    TelemetryRecord r{}; r.boot_id = 4; r.sequence_id = 10;
    journal.observePush(buffer.push(r), 0); // unavailable incoming record
    TEST_ASSERT_TRUE(buffer.begin(2));
    r.sequence_id = 11; journal.observePush(buffer.push(r), 1);
    r.sequence_id = 12; journal.observePush(buffer.push(r), 2);
    r.sequence_id = 13; journal.observePush(buffer.push(r), 3); // loses 11
    TelemetryRecord pinned{}; TEST_ASSERT_TRUE(buffer.acquirePin(pinned));
    r.sequence_id = 14; journal.observePush(buffer.push(r), 4); // loses 13, not pinned 12
    TEST_ASSERT_TRUE(buffer.commitPinnedHead(4, 12));
    TEST_ASSERT_TRUE(buffer.end());
    TEST_ASSERT_TRUE(buffer.begin(1));
    r.sequence_id = 15; buffer.push(r); buffer.acquirePin(pinned);
    r.sequence_id = 16; journal.observePush(buffer.push(r), 5); // rejects incoming 16
    buffer.commitPinnedHead(4, 15); buffer.end();
    journal.flush();
    expectRange(journal, 10, 10, 1, LossPolicy::BUFFER_UNAVAILABLE, 4);
    expectRange(journal, 11, 11, 1, LossPolicy::EVICTED_OLDEST, 4);
    expectRange(journal, 13, 13, 1, LossPolicy::EVICTED_PROTECTED, 4);
    expectRange(journal, 16, 16, 1, LossPolicy::REJECTED_NEWEST, 4);
}

static void test_concurrent_producers_consumer_and_expiry_account_exactly() {
    LossJournal journal; TEST_ASSERT_TRUE(journal.begin(8, 1, 100));
    constexpr uint32_t count = 2000;
    std::atomic<unsigned> done{0};
    std::atomic<bool> start{false}, accepted{true};
    auto producer = [&](uint32_t boot) {
        while (!start.load()) std::this_thread::yield();
        for (uint32_t seq = 0; seq < count; ++seq) {
            if (!journal.observePush(loss(seq, PushResult::NOT_INITIALISED, boot), 0)) accepted = false;
            std::this_thread::yield();
        }
        ++done;
    };
    std::thread first(producer, 1), second(producer, 2);
    std::vector<JournalEvent> events;
    start = true;
    while (done.load() != 2) {
        journal.closeExpired(100);
        JournalEvent event{};
        if (journal.pop(event)) events.push_back(event);
        std::this_thread::yield();
    }
    first.join(); second.join();
    JournalEvent event{};
    while (journal.pop(event)) events.push_back(event);
    TEST_ASSERT_TRUE(accepted.load());
    std::vector<bool> seen(count * 2, false);
    for (const auto& e : events) {
        TEST_ASSERT_EQUAL_INT((int)JournalEventKind::LOSS, (int)e.kind);
        TEST_ASSERT_EQUAL_UINT32(1, e.loss.count);
        TEST_ASSERT_TRUE(e.loss.boot_id == 1 || e.loss.boot_id == 2);
        TEST_ASSERT_LESS_THAN_UINT32(count, e.loss.first_seq);
        TEST_ASSERT_EQUAL_UINT32(e.loss.first_seq, e.loss.last_seq);
        const auto index = (e.loss.boot_id - 1) * count + e.loss.first_seq;
        TEST_ASSERT_FALSE(seen[index]); seen[index] = true;
    }
    const auto stats = journal.getStats();
    TEST_ASSERT_EQUAL_UINT64(count * 2, stats.buffer_unavailable);
    TEST_ASSERT_EQUAL_UINT64(count * 2, events.size() + stats.diagnostic_events_lost);
    TEST_ASSERT_EQUAL_UINT(0, stats.queued);
}

static void test_concurrent_open_ranges_preserve_exact_membership() {
    LossJournal journal; TEST_ASSERT_TRUE(journal.begin(64, 128, 100));
    constexpr uint32_t count = 20; // <=64 events even if every range is a singleton
    std::atomic<unsigned> done{0};
    std::atomic<bool> start{false};
    auto producer = [&](uint32_t boot) {
        while (!start.load()) std::this_thread::yield();
        for (uint32_t seq = 0; seq < count; ++seq) {
            journal.observePush(loss(seq, PushResult::EVICTED_PROTECTED, boot), 0);
            std::this_thread::yield();
        }
        ++done;
    };
    std::thread a(producer, 1), b(producer, 2);
    start = true;
    while (done.load() != 2) {
        journal.closeExpired(100); // competes with range extension/splitting
        std::this_thread::yield();
    }
    a.join(); b.join(); journal.flush();
    std::vector<unsigned> seen(count * 2, 0);
    JournalEvent event{};
    while (journal.pop(event)) {
        TEST_ASSERT_EQUAL_INT((int)JournalEventKind::LOSS, (int)event.kind);
        const auto& range = event.loss;
        TEST_ASSERT_TRUE(range.boot_id == 1 || range.boot_id == 2);
        TEST_ASSERT_EQUAL_INT((int)LossPolicy::EVICTED_PROTECTED, (int)range.policy);
        TEST_ASSERT_TRUE(range.count > 0 && range.count <= count);
        TEST_ASSERT_EQUAL_UINT32(range.first_seq + range.count - 1, range.last_seq);
        for (uint32_t offset = 0; offset < range.count; ++offset) {
            const uint32_t seq = range.first_seq + offset;
            TEST_ASSERT_LESS_THAN_UINT32(count, seq);
            ++seen[(range.boot_id - 1) * count + seq];
        }
    }
    for (auto n : seen) TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT64(0, journal.getStats().diagnostic_events_lost);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_lifecycle_validation_and_ok_noop);
    RUN_TEST(test_adjacent_only_compression_in_observation_order);
    RUN_TEST(test_span_and_timer_close_without_another_loss);
    RUN_TEST(test_timer_not_extended_and_clock_wrap);
    RUN_TEST(test_overflow_counts_events_not_records_and_queue_wraps);
    RUN_TEST(test_abandonment_is_outcome_not_confirmed_loss);
    RUN_TEST(test_abandonment_counters_survive_journal_overflow);
    RUN_TEST(test_real_buffer_outcomes_forward_exact_victims);
    RUN_TEST(test_concurrent_producers_consumer_and_expiry_account_exactly);
    RUN_TEST(test_concurrent_open_ranges_preserve_exact_membership);
    return UNITY_END();
}
