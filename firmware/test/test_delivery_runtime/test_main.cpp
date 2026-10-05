#include <unity.h>
#include <DeliveryRuntime.h>
#include <vector>
#include <functional>

void setUp() {}
void tearDown() {}

struct RuntimeBuffer : BufferManager {
    bool failAbandon = false;
    unsigned pressureChecks = 0;
    bool validatePinnedHead(uint32_t boot, uint32_t seq) {
        return !failAbandon && BufferManager::validatePinnedHead(boot, seq);
    }
    AbandonResult abandonPinnedHeadIfFull(uint32_t boot, uint32_t seq, RecordIdentity& out) {
        ++pressureChecks;
        return failAbandon ? AbandonResult::INVALID_PIN
                          : BufferManager::abandonPinnedHeadIfFull(boot, seq, out);
    }
    ~RuntimeBuffer() {
        TelemetryRecord r{};
        if (peek(r)) commitPinnedHead(r.boot_id, r.sequence_id);
        end();
    }
};
struct Rig {
    RuntimeBuffer buffer;
    LossJournal journal;
    DeliveryRuntime<RuntimeBuffer> runtime;
    bool subscribeOK = true, publishOK = true;
    uint32_t now = 0, subscribeDelay = 0, publishDuration = 0;
    std::vector<uint32_t> subscriptions, publishes;
    std::vector<RecordIdentity> sent;
    std::function<void(RecordIdentity)> onPublish;
    explicit Rig(LeaseConfig cfg = {}, size_t capacity = 128) : runtime(buffer, journal, cfg) {
        TEST_ASSERT_TRUE(buffer.begin(capacity)); TEST_ASSERT_TRUE(journal.begin());
    }
    void push(uint32_t seq) {
        TelemetryRecord record{}; record.boot_id = 7; record.sequence_id = seq;
        const auto result = buffer.push(record);
        TEST_ASSERT_FALSE(result.record_lost);
    }
    void step(uint32_t at, bool up = true, const RecordIdentity* ack = nullptr) {
        now = at;
        runtime.step(now, up, ack, [&]() {
            subscriptions.push_back(now); now += subscribeDelay; return subscribeOK;
        }, [&](const TelemetryRecord& r) {
            publishes.push_back(now); sent.push_back({r.boot_id, r.sequence_id});
            if (onPublish) onPublish(sent.back());
            now += publishDuration;
            return publishOK;
        }, [&]() { return now; });
    }
};

static void test_connect_without_event_leaves_buffering_and_delivers() {
    Rig r; r.push(1); r.step(0, false);
    TEST_ASSERT_EQUAL_INT((int)NodeState::BUFFERING, (int)r.runtime.mode());
    r.step(10); // no connect callback or cadence tick
    TEST_ASSERT_EQUAL_UINT(1, r.publishes.size());
    TEST_ASSERT_EQUAL_INT((int)NodeState::SYNCING, (int)r.runtime.mode());
    TEST_ASSERT_TRUE(r.runtime.subscriptionSent());
}

static void test_connect_signal_followed_by_wifi_down_recovers_without_another_edge() {
    Rig r; r.push(1);
    r.runtime.newSession(0); // callback happened, but WiFi drops before post-loop check
    r.step(0, false); r.step(100, true);
    TEST_ASSERT_EQUAL_UINT(1, r.publishes.size());
    TEST_ASSERT_EQUAL_UINT(1, r.subscriptions.size());
    TEST_ASSERT_EQUAL_INT((int)NodeState::SYNCING, (int)r.runtime.mode());
}

static void test_new_session_invalidates_subscription_even_without_sampled_disconnect() {
    Rig r; r.push(1); r.step(0);
    r.runtime.newSession(100);
    r.subscribeOK = false; r.step(100);
    TEST_ASSERT_FALSE(r.runtime.subscriptionSent());
    TEST_ASSERT_EQUAL_UINT(1, r.publishes.size());
    TEST_ASSERT_EQUAL_UINT(2, r.subscriptions.size());
    TEST_ASSERT_EQUAL_UINT32(100, r.runtime.lease().state().connected_wait_ms);
    r.subscribeOK = true; r.step(1100);
    TEST_ASSERT_EQUAL_UINT(2, r.publishes.size());
    TEST_ASSERT_EQUAL_UINT32(100, r.runtime.lease().state().connected_wait_ms);
}

static void test_subscription_failures_back_off_cap_and_never_publish_or_abandon() {
    Rig r; r.push(1); r.subscribeOK = false;
    for (uint32_t t = 0; t <= 92000; t += 10) r.step(t, true);
    const uint32_t expected[] = {0,1000,3000,7000,15000,31000,61000,91000};
    TEST_ASSERT_EQUAL_UINT(8, r.subscriptions.size());
    for (unsigned i=0;i<8;++i) TEST_ASSERT_EQUAL_UINT32(expected[i], r.subscriptions[i]);
    TEST_ASSERT_EQUAL_UINT(0, r.publishes.size());
    TEST_ASSERT_TRUE(r.runtime.lease().idle());
    TEST_ASSERT_EQUAL_UINT(1, r.buffer.available());
    TEST_ASSERT_EQUAL_UINT(0, r.buffer.getStats().abandoned);
    TEST_ASSERT_EQUAL_UINT64(8, r.runtime.subscriptionCounters().failures);
}

static void test_active_pin_budget_pauses_during_subscription_failure_and_blocking_setup() {
    Rig r; r.push(1); r.step(0);
    r.runtime.observeTransport(100, false); // stop time BEFORE network work
    r.subscribeOK = false;
    r.step(21600100); // six-hour outage, then connected but setup fails
    r.step(21601100);
    TEST_ASSERT_EQUAL_UINT(1, r.publishes.size());
    TEST_ASSERT_EQUAL_UINT32(100, r.runtime.lease().state().connected_wait_ms);
    TEST_ASSERT_FALSE(r.buffer.clear()); // original record remains pinned
    r.subscribeOK = true; r.subscribeDelay = 2000;
    r.step(21603100);
    TEST_ASSERT_EQUAL_UINT(2, r.publishes.size());
    TEST_ASSERT_EQUAL_UINT32(100, r.runtime.lease().state().connected_wait_ms);
    TEST_ASSERT_EQUAL_UINT32(1, r.sent.back().sequence_id);
}

static void test_matching_ack_is_accepted_while_subscription_not_ready() {
    Rig r; r.push(1); r.step(0);
    r.runtime.newSession(100); r.subscribeOK = false;
    const RecordIdentity ack{7,1}; r.step(100, true, &ack);
    TEST_ASSERT_TRUE(r.runtime.lease().idle());
    TEST_ASSERT_TRUE(r.buffer.isEmpty());
    TEST_ASSERT_EQUAL_UINT(1, r.publishes.size());
    TEST_ASSERT_EQUAL_UINT64(1, r.runtime.lease().counters().committed);
}

static void test_150ms_ack_latency_drains_every_150ms() {
    Rig r; for(uint32_t seq=0;seq<20;++seq) r.push(seq);
    uint32_t ackAt=UINT32_MAX; RecordIdentity ack{};
    r.onPublish = [&](RecordIdentity id) { ack=id; ackAt=r.now+150; };
    for(uint32_t t=0;t<=1000;t+=10)
        r.step(t,true,t==ackAt?&ack:nullptr);
    TEST_ASSERT_EQUAL_UINT(7, r.publishes.size());
    for(unsigned n=0;n<7;++n) TEST_ASSERT_EQUAL_UINT32(n*150, r.publishes[n]);
}

static void test_ack_resolution_reuses_send_opportunity_but_never_bursts() {
    Rig r; r.push(1); r.push(2); r.push(3); r.step(0);
    const RecordIdentity first{7,1}, second{7,2};
    r.step(50,true,&first); // early ACK cannot bypass 100ms minimum
    TEST_ASSERT_EQUAL_UINT(1,r.publishes.size());
    r.step(100); TEST_ASSERT_EQUAL_UINT(2,r.publishes.size());
    r.step(200,true,&second);
    TEST_ASSERT_EQUAL_UINT(3,r.publishes.size());
    TEST_ASSERT_EQUAL_UINT32(200,r.publishes.back());
}

static void test_normal_timeout_backlog_returns_to_normal_after_catchup() {
    Rig r; r.step(0); // ready and empty -> NORMAL
    uint32_t nextSeq=0, ackAt=UINT32_MAX; RecordIdentity ack{};
    r.onPublish = [&](RecordIdentity id) {
        ack=id;
        ackAt=r.publishes.size()==1 ? UINT32_MAX : r.now+50; // lose first ACK only
    };
    bool sawCatchup=false;
    for(uint32_t t=10;t<=15000;t+=10) {
        if(t%500==0) r.push(nextSeq++); // continuous 2Hz production
        r.step(t,true,t==ackAt?&ack:nullptr);
        if(r.runtime.mode()==NodeState::SYNCING && r.buffer.available()>1) sawCatchup=true;
    }
    TEST_ASSERT_TRUE(sawCatchup);
    TEST_ASSERT_EQUAL_INT((int)NodeState::NORMAL,(int)r.runtime.mode());
    TEST_ASSERT_TRUE(r.buffer.available()<=1);
    TEST_ASSERT_EQUAL_UINT64(1,r.runtime.lease().counters().retries);
    TEST_ASSERT_EQUAL_UINT64(1,r.runtime.lease().counters().ack_timeouts);
    TEST_ASSERT_EQUAL_UINT(0,r.buffer.droppedCount());
}

static void test_repeated_invariant_failure_keeps_pin_and_rate_limits_reporting() {
    LeaseConfig config; config.rapid_attempts=1;
    config.ack_timeout_ms=1;
    Rig r(config,1); r.push(1); r.step(0); r.buffer.failAbandon=true;
    unsigned reports=0;
    for(uint32_t t=0;t<=60000;t+=10) {
        r.step(t,true);
        if(r.runtime.reportDue(t)) ++reports;
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1,r.publishes.size(),"invalid ownership must block slow retries");
    }
    TEST_ASSERT_EQUAL_UINT(13,reports);
    TEST_ASSERT_EQUAL_UINT64(60,r.runtime.lease().counters().lease_internal_errors);
    TEST_ASSERT_EQUAL_UINT(1,r.buffer.available());
    TEST_ASSERT_EQUAL_UINT(0,r.buffer.getStats().abandoned);
    TEST_ASSERT_FALSE(r.buffer.clear());
    r.buffer.failAbandon=false; r.step(60010);
    TEST_ASSERT_TRUE(r.runtime.lease().idle());
    TEST_ASSERT_EQUAL_UINT(1,r.buffer.getStats().abandoned);
}

static void test_subscription_and_report_timers_wrap() {
    Rig r; r.subscribeOK=false; r.push(1);
    const uint32_t start=UINT32_MAX-499;
    r.step(start); r.step(499); TEST_ASSERT_EQUAL_UINT(1,r.subscriptions.size());
    r.step(500); TEST_ASSERT_EQUAL_UINT(2,r.subscriptions.size());
    TEST_ASSERT_TRUE(r.runtime.reportDue(start));
    TEST_ASSERT_FALSE(r.runtime.reportDue(4499));
    TEST_ASSERT_TRUE(r.runtime.reportDue(4500));
}

static void test_shared_production_configuration_and_observable_counters() {
    LeaseConfig config;
    TEST_ASSERT_EQUAL_UINT32(MQTT_ACK_TIMEOUT_MS,config.ack_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(DELIVERY_RAPID_ATTEMPTS,config.rapid_attempts);
    TEST_ASSERT_EQUAL_UINT32(DELIVERY_RETRY_BACKOFF_MS,config.retry_backoff_ms);
    TEST_ASSERT_EQUAL_UINT32(DELIVERY_SLOW_RETRY_MS,config.slow_retry_ms);
    Rig r; r.push(1); r.publishOK=false; r.step(0);
    const RecordIdentity same{7,1}, wrong{7,2};
    r.step(10,true,&same); r.step(20,true,&wrong);
    r.step(1000);
    const auto& c=r.runtime.lease().counters();
    TEST_ASSERT_EQUAL_UINT64(1,c.ack_stale);
    TEST_ASSERT_EQUAL_UINT64(1,c.ack_mismatch);
    TEST_ASSERT_EQUAL_UINT64(2,c.publish_calls);
    TEST_ASSERT_EQUAL_UINT64(2,c.publish_failures);
    TEST_ASSERT_EQUAL_UINT64(1,c.retries);
}

static void test_healthy_production_has_no_scheduling_drift() {
    // Independent producer and network clocks: work on core 0 must not slow
    // simulated production on core 1. Run long enough to expose accumulated drift.
    for (uint32_t phase : {10U, 250U, 500U}) {
        Rig r;
        uint32_t now=0, nextProduction=phase;
        uint32_t ackAt=0, seq=0, catchups=0, iteration=0;
        bool pendingAck=false;
        RecordIdentity pending{};
        auto produce = [&]() {
            while (nextProduction<=now) {
                r.push(seq++);
                nextProduction+=500;
            }
        };
        while (now<600000) {
            produce();
            const auto before=r.runtime.mode();
            const RecordIdentity ack=pending;
            const bool ackReady=pendingAck && now>=ackAt;
            if (ackReady) pendingAck=false;
            r.runtime.step(now,true,ackReady?&ack:nullptr,[](){return true;},
                [&](const TelemetryRecord& record) {
                    now+=1+(record.sequence_id%4)*7; // 1/8/15/22 ms publish cost
                    produce();
                    pending={record.boot_id,record.sequence_id};
                    ackAt=now+20+(record.sequence_id%3)*65; pendingAck=true;
                    return true;
                },[&](){return now;});
            if (before==NodeState::NORMAL && r.runtime.mode()==NodeState::SYNCING) ++catchups;
            TEST_ASSERT_TRUE(r.buffer.available()<=2);
            now+=10+(iteration++%3)*3; // connection-task service jitter
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0,catchups,"healthy 2 Hz production must not need catch-up");
        TEST_ASSERT_TRUE(seq-r.runtime.lease().counters().committed<=1);
        TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().retries);
        TEST_ASSERT_EQUAL_UINT(0,r.buffer.droppedCount());
    }
}

static void test_pacing_uses_publish_start_and_does_not_bank_missed_ticks() {
    Rig r; for (uint32_t seq=1;seq<=4;++seq) r.push(seq);
    r.publishDuration=30;
    r.step(0);
    const RecordIdentity first{7,1}, second{7,2}, third{7,3};
    r.step(60,true,&first);
    r.step(99); TEST_ASSERT_EQUAL_UINT(1,r.publishes.size());
    r.step(100); TEST_ASSERT_EQUAL_UINT(2,r.publishes.size());
    r.step(160,true,&second);
    r.step(10000); // long task-service gap: only one new record, no saved ticks
    TEST_ASSERT_EQUAL_UINT(3,r.publishes.size());
    r.step(10060,true,&third);
    r.step(10099); TEST_ASSERT_EQUAL_UINT(3,r.publishes.size());
    r.step(10100); TEST_ASSERT_EQUAL_UINT(4,r.publishes.size());
    for (size_t i=1;i<r.publishes.size();++i)
        TEST_ASSERT_TRUE(r.publishes[i]-r.publishes[i-1]>=DELIVERY_MIN_PUBLISH_INTERVAL_MS);
}

static void test_publish_pacing_wraps_with_nonzero_duration() {
    Rig r; r.push(1); r.push(2); r.publishDuration=30;
    r.step(UINT32_MAX-49);
    const RecordIdentity first{7,1};
    r.step(0,true,&first);
    r.step(49); TEST_ASSERT_EQUAL_UINT(1,r.publishes.size());
    r.step(50); TEST_ASSERT_EQUAL_UINT(2,r.publishes.size());
}

static void test_brief_mode_changes_remain_visible_until_consumed() {
    Rig r; r.step(0); // empty ready buffer visits SYNCING then NORMAL
    const uint8_t normal=1U<<static_cast<unsigned>(NodeState::NORMAL);
    const uint8_t buffering=1U<<static_cast<unsigned>(NodeState::BUFFERING);
    const uint8_t syncing=1U<<static_cast<unsigned>(NodeState::SYNCING);
    TEST_ASSERT_EQUAL_UINT8(normal|syncing,r.runtime.takeModeChanges());
    TEST_ASSERT_EQUAL_UINT8(0,r.runtime.takeModeChanges());
    r.runtime.observeTransport(10,false);
    r.step(20);
    TEST_ASSERT_EQUAL_UINT8(buffering|syncing|normal,r.runtime.takeModeChanges());
    r.step(30); TEST_ASSERT_EQUAL_UINT8(0,r.runtime.takeModeChanges());
}

static void test_bridge_outage_retains_records_and_recovers_without_reconnect() {
    Rig r({},2000);
    uint32_t seq=0;
    // Broker and subscription stay up, but no application ACK arrives.
    for (uint32_t t=0;t<=600000;t+=10) {
        if (t%500==0) r.push(seq++);
        r.step(t);
    }
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
    TEST_ASSERT_EQUAL_UINT(seq,r.buffer.available());
    TEST_ASSERT_TRUE(r.publishes.size()>5); // attempt cap must not stall recovery
    TEST_ASSERT_TRUE(r.publishes.size()<=25); // bounded slow probes, not a storm
    for (auto id : r.sent) TEST_ASSERT_EQUAL_UINT32(0,id.sequence_id);
    uint32_t ackAt=UINT32_MAX; RecordIdentity ack{};
    r.onPublish=[&](RecordIdentity id) { ack=id; ackAt=r.now+50; };
    // Bridge recovers without an MQTT reconnect. Stop producing and drain.
    for (uint32_t t=600010;t<=800000;t+=10)
        r.step(t,true,t==ackAt?&ack:nullptr);
    TEST_ASSERT_TRUE(r.buffer.isEmpty());
    TEST_ASSERT_EQUAL_UINT64(seq,r.runtime.lease().counters().committed);
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
    TEST_ASSERT_EQUAL_UINT(1,r.subscriptions.size());
}

static void test_new_sessions_grant_fresh_attempt_without_resetting_pacing() {
    LeaseConfig cfg; cfg.rapid_attempts=1;
    Rig r(cfg,1); r.push(1); r.step(0);
    for (uint32_t t : {100U,200U,300U}) {
        r.runtime.newSession(t); r.step(t);
        TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
        TEST_ASSERT_EQUAL_UINT(1,r.publishes.size());
    }
    r.step(29999); TEST_ASSERT_EQUAL_UINT(1,r.publishes.size());
    r.step(30000); TEST_ASSERT_EQUAL_UINT(2,r.publishes.size());
    TEST_ASSERT_EQUAL_UINT32(2,r.runtime.lease().state().publish_calls);
    r.step(32999); TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
    const RecordIdentity ack{7,1}; r.step(33000,true,&ack);
    TEST_ASSERT_TRUE(r.buffer.isEmpty());
    TEST_ASSERT_EQUAL_UINT64(1,r.runtime.lease().counters().committed);
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
}

static void test_full_buffer_after_outage_gets_fresh_attempt_and_complete_ack_window() {
    Rig r({},3); r.push(1);
    for (uint32_t t=0;t<=15000;t+=10) r.step(t);
    TEST_ASSERT_EQUAL_UINT(5,r.publishes.size());
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
    r.runtime.observeTransport(15000,false);
    r.push(2); r.push(3);
    r.step(21615000,false); // six hours, pin held at capacity
    r.runtime.newSession(21615000); r.step(21615000);
    TEST_ASSERT_EQUAL_UINT(6,r.publishes.size());
    TEST_ASSERT_EQUAL_UINT32(1,r.sent.back().sequence_id);
    TEST_ASSERT_EQUAL_UINT32(6,r.runtime.lease().state().publish_calls);
    r.step(21617999);
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
    r.step(21618000);
    TEST_ASSERT_EQUAL_UINT64(1,r.runtime.lease().counters().abandoned);
    TEST_ASSERT_EQUAL_UINT32(2,r.sent.back().sequence_id); // next record progresses
    JournalEvent event{}; TEST_ASSERT_TRUE(r.journal.pop(event));
    TEST_ASSERT_EQUAL_UINT32(1,event.abandonment.identity.sequence_id);
    TEST_ASSERT_EQUAL_UINT32(6,event.abandonment.publish_calls);
    TEST_ASSERT_EQUAL_INT((int)DeliveryOutcome::UNKNOWN_AFTER_PUBLISH,(int)event.delivery_outcome);
}

static void test_capped_failed_publish_gets_fresh_paced_attempt_after_new_session() {
    LeaseConfig cfg; cfg.rapid_attempts=1;
    Rig r(cfg,1); r.push(1); r.publishOK=false; r.step(0);
    r.runtime.newSession(100); r.step(100);
    r.step(29999);
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
    TEST_ASSERT_EQUAL_UINT(1,r.publishes.size());
    r.step(30000); TEST_ASSERT_EQUAL_UINT(2,r.publishes.size());
    r.step(30001); // failed attempt is resolved; no ACK window is owed
    TEST_ASSERT_EQUAL_UINT64(1,r.runtime.lease().counters().abandoned);
    JournalEvent event{}; TEST_ASSERT_TRUE(r.journal.pop(event));
    TEST_ASSERT_EQUAL_UINT32(2,event.abandonment.publish_calls);
    TEST_ASSERT_EQUAL_INT((int)DeliveryOutcome::NOT_PUBLISHED,(int)event.delivery_outcome);
}

static void test_interrupted_ack_wait_full_buffer_same_session_requires_resolved_retry() {
    LeaseConfig cfg; cfg.rapid_attempts=1;
    Rig r(cfg,2); r.push(1); r.push(2); r.step(0);
    r.step(100,false); // ACK window interrupted, MQTT session later survives
    r.step(200);       // deliberately NO newSession() callback
    r.step(29999);
    TEST_ASSERT_EQUAL_UINT(1,r.publishes.size());
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
    TEST_ASSERT_EQUAL_UINT(0,r.buffer.pressureChecks);
    r.step(30000);
    TEST_ASSERT_EQUAL_UINT(2,r.publishes.size());
    TEST_ASSERT_EQUAL_UINT32(1,r.sent.back().sequence_id);
    r.step(32999);
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
    r.step(33000); // a complete ACK window, not the interrupted one, authorizes release
    TEST_ASSERT_EQUAL_UINT64(1,r.runtime.lease().counters().abandoned);
    TEST_ASSERT_EQUAL_UINT32(2,r.sent.back().sequence_id);
}

static void test_pressure_checks_are_paced_and_ack_is_not() {
    LeaseConfig cfg; cfg.rapid_attempts=1;
    Rig r(cfg,2); r.push(1); r.step(0);
    for(uint32_t t=10;t<=10000;t+=10) r.step(t);
    TEST_ASSERT_EQUAL_UINT(8,r.buffer.pressureChecks); // 3000 through 10000
    r.push(2); r.step(10010); // newly full; next pressure check isn't due
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
    const RecordIdentity ack{7,1}; r.step(10020,true,&ack);
    TEST_ASSERT_EQUAL_UINT64(1,r.runtime.lease().counters().committed);
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
}

static void test_pressure_check_wrap_and_bounded_detection_delay() {
    LeaseConfig cfg; cfg.rapid_attempts=1;
    Rig r(cfg,2); r.push(1); r.publishOK=false;
    const uint32_t start=UINT32_MAX-499;
    r.step(start); r.step(start+1); // NOT_FULL, begin one-second cooldown
    r.push(2); r.step(500);
    TEST_ASSERT_EQUAL_UINT(1,r.buffer.pressureChecks);
    TEST_ASSERT_EQUAL_UINT64(0,r.runtime.lease().counters().abandoned);
    r.step(501);
    TEST_ASSERT_EQUAL_UINT(2,r.buffer.pressureChecks);
    TEST_ASSERT_EQUAL_UINT64(1,r.runtime.lease().counters().abandoned);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_connect_without_event_leaves_buffering_and_delivers);
    RUN_TEST(test_connect_signal_followed_by_wifi_down_recovers_without_another_edge);
    RUN_TEST(test_new_session_invalidates_subscription_even_without_sampled_disconnect);
    RUN_TEST(test_subscription_failures_back_off_cap_and_never_publish_or_abandon);
    RUN_TEST(test_active_pin_budget_pauses_during_subscription_failure_and_blocking_setup);
    RUN_TEST(test_matching_ack_is_accepted_while_subscription_not_ready);
    RUN_TEST(test_150ms_ack_latency_drains_every_150ms);
    RUN_TEST(test_ack_resolution_reuses_send_opportunity_but_never_bursts);
    RUN_TEST(test_normal_timeout_backlog_returns_to_normal_after_catchup);
    RUN_TEST(test_repeated_invariant_failure_keeps_pin_and_rate_limits_reporting);
    RUN_TEST(test_subscription_and_report_timers_wrap);
    RUN_TEST(test_shared_production_configuration_and_observable_counters);
    RUN_TEST(test_healthy_production_has_no_scheduling_drift);
    RUN_TEST(test_pacing_uses_publish_start_and_does_not_bank_missed_ticks);
    RUN_TEST(test_publish_pacing_wraps_with_nonzero_duration);
    RUN_TEST(test_brief_mode_changes_remain_visible_until_consumed);
    RUN_TEST(test_bridge_outage_retains_records_and_recovers_without_reconnect);
    RUN_TEST(test_new_sessions_grant_fresh_attempt_without_resetting_pacing);
    RUN_TEST(test_full_buffer_after_outage_gets_fresh_attempt_and_complete_ack_window);
    RUN_TEST(test_capped_failed_publish_gets_fresh_paced_attempt_after_new_session);
    RUN_TEST(test_interrupted_ack_wait_full_buffer_same_session_requires_resolved_retry);
    RUN_TEST(test_pressure_checks_are_paced_and_ack_is_not);
    RUN_TEST(test_pressure_check_wrap_and_bounded_detection_delay);
    return UNITY_END();
}
