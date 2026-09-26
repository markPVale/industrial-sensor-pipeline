#include <unity.h>

#include "LegacyDeliveryState.h"

namespace {

class FakeBuffer {
public:
    void setOldest(const DeliveryIdentity& identity) {
        _oldest = identity;
        _hasRecord = true;
    }

    bool peek(DeliveryIdentity& out) {
        if (!_hasRecord) return false;
        out = _oldest;
        return true;
    }

    bool pop(DeliveryIdentity& out) {
        if (!peek(out)) return false;
        _hasRecord = false;
        return true;
    }

private:
    DeliveryIdentity _oldest{};
    bool _hasRecord = false;
};

LegacyAckResult deliverAck(LegacyDeliveryState& state,
                           FakeBuffer& buffer,
                           const DeliveryIdentity& ack) {
    return handleLegacyAck(
        state,
        ack,
        [&buffer](DeliveryIdentity& oldest) { return buffer.peek(oldest); },
        [&buffer](DeliveryIdentity& removed) { return buffer.pop(removed); });
}

void test_matching_ack_commits_current_head() {
    LegacyDeliveryState state;
    FakeBuffer buffer;
    const DeliveryIdentity r0{7, 100};

    buffer.setOldest(r0);
    TEST_ASSERT_TRUE(state.tryReserve());
    state.markPublished(r0, 10);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LegacyAckResult::COMMITTED),
        static_cast<uint8_t>(deliverAck(state, buffer, r0)));
    TEST_ASSERT_FALSE(state.isInFlight());

    DeliveryIdentity ignored{};
    TEST_ASSERT_FALSE(buffer.peek(ignored));
}

void test_duplicate_ack_for_r0_does_not_release_unpublished_r1() {
    LegacyDeliveryState state;
    FakeBuffer buffer;
    const DeliveryIdentity r0{7, 100};
    const DeliveryIdentity r1{7, 101};

    // R0 is published, ACKed, and removed. release() deliberately leaves the
    // legacy identity globals naming R0.
    buffer.setOldest(r0);
    TEST_ASSERT_TRUE(state.tryReserve());
    state.markPublished(r0, 10);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LegacyAckResult::COMMITTED),
        static_cast<uint8_t>(deliverAck(state, buffer, r0)));

    // R1 is now reserved/enqueued but has not reached publish(), so the active
    // flag is true while the stored identity still names R0.
    buffer.setOldest(r1);
    TEST_ASSERT_TRUE(state.tryReserve());

    // A duplicate ACK for R0 takes the legacy identity-match branch, sees R1
    // at the head, and clears R1's reservation. The desired invariant below
    // intentionally fails until the delivery lease replaces this state model.
    (void)deliverAck(state, buffer, r0);
    TEST_ASSERT_TRUE_MESSAGE(
        state.isInFlight(),
        "duplicate ACK for R0 released R1 before R1 was published");

    DeliveryIdentity oldest{};
    TEST_ASSERT_TRUE(buffer.peek(oldest));
    TEST_ASSERT_EQUAL_UINT32(r1.boot_id, oldest.boot_id);
    TEST_ASSERT_EQUAL_UINT32(r1.sequence_id, oldest.sequence_id);
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_matching_ack_commits_current_head);
    RUN_TEST(test_duplicate_ack_for_r0_does_not_release_unpublished_r1);
    return UNITY_END();
}
