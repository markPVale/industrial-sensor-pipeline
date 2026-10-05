#include <unity.h>
#include <DeliveryLease.h>
#include <functional>
#include <vector>

void setUp() {}
void tearDown() {}

struct TestBuffer : BufferManager {
    bool failCommit = false, failAbandon = false;
    bool validatePinnedHead(uint32_t boot, uint32_t seq) {
        return !failCommit && !failAbandon && BufferManager::validatePinnedHead(boot, seq);
    }
    bool commitPinnedHead(uint32_t boot, uint32_t seq) {
        return !failCommit && BufferManager::commitPinnedHead(boot, seq);
    }
    AbandonResult abandonPinnedHeadIfFull(uint32_t boot, uint32_t seq, RecordIdentity& out) {
        return failAbandon ? AbandonResult::INVALID_PIN
                          : BufferManager::abandonPinnedHeadIfFull(boot, seq, out);
    }
    ~TestBuffer() {
        TelemetryRecord r{};
        if (peek(r)) BufferManager::commitPinnedHead(r.boot_id, r.sequence_id);
        end();
    }
};
struct Rig {
    TestBuffer buffer;
    LossJournal journal;
    DeliveryLease<TestBuffer> lease;
    uint32_t now = 0, publishDuration = 0;
    bool publishOK = true;
    std::vector<TelemetryRecord> sent;
    std::function<void(const TelemetryRecord&)> duringPublish;
    Rig(size_t capacity = 3, LeaseConfig config = {}) : lease(buffer, journal, config) {
        TEST_ASSERT_TRUE(buffer.begin(capacity));
        TEST_ASSERT_TRUE(journal.begin());
    }
    PushOutcome push(uint32_t seq, uint32_t boot = 7) {
        TelemetryRecord r{}; r.boot_id = boot; r.sequence_id = seq;
        r.timestamp_ms = 1700000000000ULL + seq; r.accel_x = 1.25f;
        const auto outcome = buffer.push(r);
        journal.observePush(outcome, now);
        return outcome;
    }
    LeaseResult step(uint32_t at, bool connected = true, bool due = true,
                     const RecordIdentity* ack = nullptr) {
        now = at;
        return lease.step(now, connected, due, ack,
            [&](const TelemetryRecord& record) {
                sent.push_back(record);
                if (duringPublish) duringPublish(record);
                now += publishDuration;
                return publishOK;
            }, [&]() { return now; });
    }
};
static LeaseConfig config(uint32_t calls) {
    LeaseConfig c; c.rapid_attempts = calls; return c;
}

static void test_matching_ack_commits_current_head() {
    Rig r; r.push(100);
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::PUBLISHED, (int)r.step(0));
    TEST_ASSERT_EQUAL_UINT(1, r.buffer.available());
    const RecordIdentity ack{7, 100};
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::COMMITTED, (int)r.step(10, true, true, &ack));
    TEST_ASSERT_TRUE(r.lease.idle()); TEST_ASSERT_TRUE(r.buffer.isEmpty());
    TEST_ASSERT_EQUAL_UINT(0, r.buffer.droppedCount());
}

static void test_duplicate_ack_for_r0_does_not_release_unpublished_r1() {
    Rig r; r.push(100); r.push(101);
    const RecordIdentity r0{7, 100};
    r.step(0); r.step(10, true, true, &r0);
    // The old QUEUED window no longer exists. Inject at the stronger boundary:
    // R1 is pinned but its synchronous publish has not returned successfully.
    r.duringPublish = [&](const TelemetryRecord& record) {
        TEST_ASSERT_EQUAL_UINT32(101, record.sequence_id);
        TEST_ASSERT_FALSE(r.lease.state().everPublished());
        r.lease.onAck(r0);
        TEST_ASSERT_FALSE_MESSAGE(r.lease.idle(), "duplicate ACK for R0 released unpublished R1");
        TEST_ASSERT_EQUAL_UINT32(101, r.lease.state().identity.sequence_id);
        TelemetryRecord oldest{};
        TEST_ASSERT_TRUE(r.buffer.peek(oldest));
        TEST_ASSERT_EQUAL_UINT32(101, oldest.sequence_id);
    };
    r.step(20);
    TEST_ASSERT_EQUAL_UINT64(1, r.lease.counters().ack_mismatch);
    TEST_ASSERT_EQUAL_INT((int)LeasePhase::AWAITING_ACK, (int)r.lease.state().phase);
    TEST_ASSERT_EQUAL_UINT(1, r.buffer.available());
}

static void test_idle_duplicate_ack_cannot_commit_next_record() {
    Rig r; r.push(1); r.push(2);
    const RecordIdentity ack{7, 1}; r.step(0); r.step(1, true, true, &ack);
    r.step(2, true, true, &ack); // stale ACK then fresh acquisition in one iteration
    TEST_ASSERT_EQUAL_UINT64(1, r.lease.counters().ack_stale);
    TEST_ASSERT_EQUAL_UINT32(2, r.lease.state().identity.sequence_id);
    TEST_ASSERT_EQUAL_UINT(1, r.buffer.available());
}

static void test_overflow_before_publish_and_before_ack_preserves_exact_record() {
    Rig r(2); r.push(1); r.push(2);
    r.duringPublish = [&](const TelemetryRecord& record) {
        const auto loss = r.push(3);
        TEST_ASSERT_EQUAL_UINT32(2, loss.lost.sequence_id);
        TEST_ASSERT_EQUAL_UINT32(1, record.sequence_id);
        TEST_ASSERT_EQUAL_UINT64(1700000000001ULL, record.timestamp_ms);
    };
    r.step(0); r.duringPublish = nullptr;
    TEST_ASSERT_EQUAL_UINT32(3, r.push(4).lost.sequence_id);
    const RecordIdentity ack{7, 1}; r.step(10, true, false, &ack);
    TelemetryRecord next{}; TEST_ASSERT_TRUE(r.buffer.peek(next));
    TEST_ASSERT_EQUAL_UINT32(4, next.sequence_id);
    TEST_ASSERT_EQUAL_UINT(2, r.buffer.getStats().protected_evictions);
}

static void test_capacity_one_rejects_newest_through_disconnect() {
    Rig r(1); r.push(1); r.step(0); r.step(100, false);
    const auto lost = r.push(2);
    TEST_ASSERT_EQUAL_INT((int)PushResult::REJECTED_NEWEST, (int)lost.result);
    TEST_ASSERT_EQUAL_UINT32(2, lost.lost.sequence_id);
    r.step(20000, false);
    TEST_ASSERT_EQUAL_UINT(1, r.buffer.available());
    TEST_ASSERT_EQUAL_UINT32(100, r.lease.state().connected_wait_ms);
}

static void test_timeout_republishes_same_identity_and_late_ack_commits() {
    Rig r; r.push(1); r.step(0);
    r.step(2999); TEST_ASSERT_EQUAL_UINT(1, r.sent.size());
    r.step(3000); TEST_ASSERT_EQUAL_UINT(2, r.sent.size());
    TEST_ASSERT_EQUAL_UINT32(r.sent[0].sequence_id, r.sent[1].sequence_id);
    TEST_ASSERT_EQUAL_UINT64(r.sent[0].timestamp_ms, r.sent[1].timestamp_ms);
    r.lease.setConnected(3001, false); // RETRY_PENDING still ACK-eligible
    const RecordIdentity ack{7, 1};
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::COMMITTED, (int)r.step(3100, false, false, &ack));
    TEST_ASSERT_TRUE(r.buffer.isEmpty()); TEST_ASSERT_EQUAL_UINT(2, r.sent.size());
}

static void test_late_ack_after_failed_retry_uses_earlier_publish_history() {
    Rig r; r.push(1); r.step(0);
    r.publishOK = false; r.step(3000);
    TEST_ASSERT_EQUAL_INT((int)LeasePhase::RETRY_PENDING, (int)r.lease.state().phase);
    TEST_ASSERT_TRUE(r.lease.state().everPublished());
    const RecordIdentity ack{7, 1};
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::COMMITTED, (int)r.step(3100, true, true, &ack));
    TEST_ASSERT_EQUAL_UINT(2, r.sent.size());
    TEST_ASSERT_TRUE(r.buffer.isEmpty());
}

static void test_ack_never_published_and_mismatch_change_no_lease_state() {
    Rig r; r.push(1); r.publishOK = false; r.step(0);
    const LeaseState before = r.lease.state();
    r.lease.onAck({7, 1}); r.lease.onAck({8, 1}); r.lease.onAck({7, 2});
    TEST_ASSERT_EQUAL_UINT64(1, r.lease.counters().ack_stale);
    TEST_ASSERT_EQUAL_UINT64(2, r.lease.counters().ack_mismatch);
    TEST_ASSERT_EQUAL_INT((int)before.phase, (int)r.lease.state().phase);
    TEST_ASSERT_EQUAL_UINT32(before.successful_publishes, r.lease.state().successful_publishes);
    TEST_ASSERT_EQUAL_UINT32(before.publish_calls, r.lease.state().publish_calls);
    TEST_ASSERT_EQUAL_UINT(1, r.buffer.available());
    r.step(999); TEST_ASSERT_EQUAL_UINT(1, r.sent.size());
    r.step(1000); TEST_ASSERT_EQUAL_UINT(2, r.sent.size());
}

static void test_publish_failure_backoff_and_bounded_unpublished_abandonment() {
    Rig r(2); r.push(1); r.push(2); r.publishOK = false;
    for (auto at : {0U, 1000U, 3000U, 7000U, 15000U}) {
        TEST_ASSERT_EQUAL_INT((int)LeaseResult::PUBLISH_FAILED, (int)r.step(at));
        r.step(at + 1); // may abandon after the fifth call, never a retry storm
    }
    TEST_ASSERT_EQUAL_UINT(5, r.sent.size()); TEST_ASSERT_TRUE(r.lease.idle());
    TEST_ASSERT_EQUAL_UINT(1, r.buffer.getStats().abandoned);
    TEST_ASSERT_EQUAL_UINT(0, r.buffer.droppedCount());
    JournalEvent event{}; TEST_ASSERT_TRUE(r.journal.pop(event));
    TEST_ASSERT_EQUAL_INT((int)DeliveryOutcome::NOT_PUBLISHED, (int)event.delivery_outcome);
    TEST_ASSERT_EQUAL_UINT32(5, event.abandonment.publish_calls);
    TEST_ASSERT_EQUAL_UINT32(1, event.abandonment.identity.sequence_id);
    TEST_ASSERT_EQUAL_UINT64(1, r.journal.getStats().lease_abandoned_unpublished);
    r.publishOK = true; r.step(15002); // next record can now make progress
    TEST_ASSERT_EQUAL_UINT32(2, r.sent.back().sequence_id);
}

static void test_exhausted_attempts_keep_slow_retrying_with_free_space() {
    Rig r(3, config(2)); r.push(1); r.publishOK = false;
    r.step(0); r.step(1000);
    for (uint32_t t = 1100; t < 31000; t += 100) r.step(t);
    TEST_ASSERT_EQUAL_UINT(2, r.sent.size());
    TEST_ASSERT_FALSE(r.lease.idle());
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::PUBLISH_FAILED, (int)r.step(31000));
    TEST_ASSERT_EQUAL_UINT(3, r.sent.size());
    TEST_ASSERT_EQUAL_UINT64(0, r.lease.counters().abandoned);
    TEST_ASSERT_EQUAL_UINT64(0, r.lease.counters().lease_internal_errors);
}

static void test_successful_publishes_without_ack_abandon_as_unknown() {
    Rig r(1); r.push(1);
    for (uint32_t t = 0; t <= 12000; t += 3000) r.step(t);
    r.step(14999); TEST_ASSERT_EQUAL_UINT(5, r.sent.size());
    TEST_ASSERT_FALSE(r.lease.idle()); // fifth attempt still has its ACK window
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::ABANDONED, (int)r.step(15000));
    JournalEvent event{}; TEST_ASSERT_TRUE(r.journal.pop(event));
    TEST_ASSERT_EQUAL_INT((int)DeliveryOutcome::UNKNOWN_AFTER_PUBLISH, (int)event.delivery_outcome);
    TEST_ASSERT_EQUAL_UINT32(5, event.abandonment.successful_publishes);
    TEST_ASSERT_EQUAL_UINT64(1, r.journal.getStats().lease_abandoned_after_publish);
}

static void test_disconnect_freezes_budget_and_reconnect_retries_without_timeout() {
    Rig r; r.push(1); r.step(100);
    r.lease.setConnected(200, false); // observed edge BEFORE blocking reconnect
    r.step(21600200, false); // six hours
    TEST_ASSERT_EQUAL_UINT32(100, r.lease.state().connected_wait_ms);
    TEST_ASSERT_EQUAL_UINT(1, r.sent.size());
    r.lease.setConnected(21600250, true); // onConnect completion
    r.step(21600250);
    TEST_ASSERT_EQUAL_UINT32(100, r.lease.state().connected_wait_ms);
    TEST_ASSERT_EQUAL_UINT(2, r.sent.size());
    TEST_ASSERT_EQUAL_UINT32(1, r.sent.back().sequence_id);
}

static void test_failed_publish_backoff_survives_reconnect() {
    Rig r; r.push(1); r.publishOK = false; r.step(0);
    r.step(100, false); r.step(500, true);
    TEST_ASSERT_EQUAL_UINT(1, r.sent.size());
    r.step(1000); TEST_ASSERT_EQUAL_UINT(2, r.sent.size());
    r.step(1001, false); r.step(1500, true);
    TEST_ASSERT_EQUAL_UINT(2, r.sent.size());
    r.step(2999); TEST_ASSERT_EQUAL_UINT(2, r.sent.size());
    r.step(3000); TEST_ASSERT_EQUAL_UINT(3, r.sent.size());
}

static void test_ack_wins_over_timeout_and_abandonment() {
    Rig r(1, config(1)); r.push(1); r.step(0);
    const RecordIdentity ack{7, 1};
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::COMMITTED, (int)r.step(3000, true, true, &ack));
    TEST_ASSERT_EQUAL_UINT(1, r.sent.size());
    TEST_ASSERT_EQUAL_UINT(0, r.buffer.getStats().abandoned);
    TEST_ASSERT_EQUAL_UINT(0, r.journal.getStats().queued);
}

static void test_disconnect_and_ack_same_iteration_commits() {
    Rig r; r.push(1); r.step(0);
    const RecordIdentity ack{7, 1};
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::COMMITTED, (int)r.step(10, false, true, &ack));
    TEST_ASSERT_TRUE(r.buffer.isEmpty());
}

static void test_failed_buffer_mutations_preserve_pin_and_state() {
    Rig r(1, config(1)); r.push(1); r.step(0);
    const RecordIdentity ack{7, 1}; r.buffer.failCommit = true;
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::INTERNAL_ERROR, (int)r.step(3000, true, true, &ack));
    TEST_ASSERT_EQUAL_INT((int)LeasePhase::AWAITING_ACK, (int)r.lease.state().phase);
    TEST_ASSERT_EQUAL_UINT32(1, r.lease.state().identity.sequence_id);
    TEST_ASSERT_EQUAL_UINT(1, r.buffer.available());
    // Ownership validation blocks timers/publishing until the pin is trusted.
    r.buffer.failAbandon = true;
    const auto calls = r.lease.state().publish_calls;
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::NONE, (int)r.step(3001));
    TEST_ASSERT_EQUAL_INT((int)LeaseResult::INTERNAL_ERROR, (int)r.step(4000));
    TEST_ASSERT_EQUAL_INT((int)LeasePhase::AWAITING_ACK, (int)r.lease.state().phase);
    TEST_ASSERT_EQUAL_UINT32(calls, r.lease.state().publish_calls);
    TEST_ASSERT_EQUAL_UINT64(2, r.lease.counters().lease_internal_errors);
    TEST_ASSERT_EQUAL_UINT(0, r.buffer.getStats().abandoned);
    TEST_ASSERT_FALSE(r.buffer.clear());
    r.buffer.failCommit = false; r.step(4001, true, true, &ack);
    TEST_ASSERT_TRUE(r.lease.idle());
}

static void test_wraparound_and_blocking_publish_start_ack_timer_on_completion() {
    Rig r; r.push(1); r.publishDuration = 50;
    r.step(UINT32_MAX - 20);
    TEST_ASSERT_EQUAL_UINT32(29, r.lease.state().sent_at_ms);
    TEST_ASSERT_EQUAL_UINT32(50, r.lease.state().connected_wait_ms);
    r.publishDuration = 0;
    r.step(3028); TEST_ASSERT_EQUAL_UINT(1, r.sent.size());
    r.step(3029); TEST_ASSERT_EQUAL_UINT(2, r.sent.size());
    Rig failed; failed.push(1); failed.publishOK = false;
    failed.step(UINT32_MAX - 499);
    failed.step(499); TEST_ASSERT_EQUAL_UINT(1, failed.sent.size());
    failed.step(500); TEST_ASSERT_EQUAL_UINT(2, failed.sent.size());
}

static void test_due_empty_and_disconnected_do_not_acquire() {
    Rig r; r.step(0); TEST_ASSERT_TRUE(r.lease.idle());
    r.push(1); r.step(1, true, false); r.step(2, false, true);
    TEST_ASSERT_TRUE(r.lease.idle()); TEST_ASSERT_EQUAL_UINT(0, r.sent.size());
    TelemetryRecord out{}; TEST_ASSERT_TRUE(r.buffer.pop(out)); // no pin was taken
}

static void test_slow_probe_countdown_survives_wrap_and_new_session() {
    Rig r(3,config(1)); r.push(1); r.publishOK=false;
    r.step(UINT32_MAX-999);
    r.step(UINT32_MAX-499,false);
    r.lease.onNewSession();
    r.step(28999); TEST_ASSERT_EQUAL_UINT(1,r.sent.size());
    r.step(29000); TEST_ASSERT_EQUAL_UINT(2,r.sent.size());
    TEST_ASSERT_EQUAL_UINT32(2,r.lease.state().publish_calls);
    TEST_ASSERT_EQUAL_UINT64(0,r.lease.counters().abandoned);
    TEST_ASSERT_EQUAL_UINT32(1,r.sent.back().sequence_id);
}

static void test_failed_commit_blocks_retries_across_sessions_until_revalidated() {
    Rig r(3); r.push(1); r.step(0);
    const RecordIdentity ack{7,1}; r.buffer.failCommit=true;
    r.step(10,true,true,&ack);
    TEST_ASSERT_TRUE(r.lease.ownershipFault());
    for(uint32_t t=20;t<=60000;t+=10) {
        if(t==10000 || t==30000) r.lease.onNewSession();
        r.step(t);
        TEST_ASSERT_EQUAL_UINT(1,r.sent.size());
    }
    TEST_ASSERT_EQUAL_UINT(1,r.buffer.available());
    r.buffer.failCommit=false;
    r.step(60010); // validate the existing pin; no implicit reacquisition
    TEST_ASSERT_FALSE(r.lease.ownershipFault());
    TEST_ASSERT_EQUAL_UINT(2,r.sent.size());
    TEST_ASSERT_EQUAL_UINT32(1,r.sent.back().sequence_id);
}

static void test_missing_pin_cannot_republish_owned_copy_or_acquire_another_record() {
    Rig r(1,config(1)); r.push(1); r.step(0);
    // Deliberately violate single-owner discipline to exercise actual pin
    // mismatch, rather than only a mocked API failure.
    TEST_ASSERT_TRUE(r.buffer.BufferManager::commitPinnedHead(7,1));
    r.push(2); TelemetryRecord other{};
    TEST_ASSERT_TRUE(r.buffer.acquirePin(other));
    for(uint32_t t=3000;t<=90000;t+=10) r.step(t);
    TEST_ASSERT_TRUE(r.lease.ownershipFault());
    TEST_ASSERT_EQUAL_UINT(1,r.sent.size());
    TEST_ASSERT_EQUAL_UINT32(1,r.lease.state().identity.sequence_id);
    TEST_ASSERT_TRUE(r.buffer.validatePinnedHead(7,2));
    TEST_ASSERT_FALSE(r.buffer.validatePinnedHead(7,1));
    TEST_ASSERT_EQUAL_UINT64(0,r.lease.counters().abandoned);
}

static void test_matching_ack_recovers_ownership_fault_during_validation_cooldown() {
    Rig r(1,config(1)); r.push(1); r.step(0);
    r.buffer.failAbandon=true; r.step(3000);
    TEST_ASSERT_TRUE(r.lease.ownershipFault());
    const RecordIdentity ack{7,1}; r.step(3010,false,false,&ack);
    TEST_ASSERT_TRUE(r.lease.idle());
    TEST_ASSERT_FALSE(r.lease.ownershipFault());
    TEST_ASSERT_TRUE(r.buffer.isEmpty());
    TEST_ASSERT_EQUAL_UINT64(1,r.lease.counters().committed);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_matching_ack_commits_current_head);
    RUN_TEST(test_duplicate_ack_for_r0_does_not_release_unpublished_r1);
    RUN_TEST(test_idle_duplicate_ack_cannot_commit_next_record);
    RUN_TEST(test_overflow_before_publish_and_before_ack_preserves_exact_record);
    RUN_TEST(test_capacity_one_rejects_newest_through_disconnect);
    RUN_TEST(test_timeout_republishes_same_identity_and_late_ack_commits);
    RUN_TEST(test_late_ack_after_failed_retry_uses_earlier_publish_history);
    RUN_TEST(test_ack_never_published_and_mismatch_change_no_lease_state);
    RUN_TEST(test_publish_failure_backoff_and_bounded_unpublished_abandonment);
    RUN_TEST(test_exhausted_attempts_keep_slow_retrying_with_free_space);
    RUN_TEST(test_successful_publishes_without_ack_abandon_as_unknown);
    RUN_TEST(test_disconnect_freezes_budget_and_reconnect_retries_without_timeout);
    RUN_TEST(test_failed_publish_backoff_survives_reconnect);
    RUN_TEST(test_ack_wins_over_timeout_and_abandonment);
    RUN_TEST(test_disconnect_and_ack_same_iteration_commits);
    RUN_TEST(test_failed_buffer_mutations_preserve_pin_and_state);
    RUN_TEST(test_wraparound_and_blocking_publish_start_ack_timer_on_completion);
    RUN_TEST(test_due_empty_and_disconnected_do_not_acquire);
    RUN_TEST(test_slow_probe_countdown_survives_wrap_and_new_session);
    RUN_TEST(test_failed_commit_blocks_retries_across_sessions_until_revalidated);
    RUN_TEST(test_missing_pin_cannot_republish_owned_copy_or_acquire_another_record);
    RUN_TEST(test_matching_ack_recovers_ownership_fault_during_validation_cooldown);
    return UNITY_END();
}
