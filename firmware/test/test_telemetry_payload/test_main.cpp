#include <unity.h>
#include <TelemetryPayload.h>
#include <config.h>
#include <float.h>
#include <string.h>
#include <initializer_list>

void setUp() {}
void tearDown() {}

static void test_finite_values_keep_wire_format() {
    TelemetryRecord r{}; r.boot_id=1; r.sequence_id=42; r.timestamp_ms=123;
    r.accel_z=9.8123f; r.window_rms=9.8123f;
    char buf[MQTT_PAYLOAD_SIZE]; TEST_ASSERT_TRUE(buildPayload(r,buf,sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("{\"boot\":1,\"seq\":42,\"ts\":123,\"ax\":0.0000,\"ay\":0.0000,\"az\":9.8123,\"gx\":0.0000,\"gy\":0.0000,\"gz\":0.0000,\"wrms\":9.8123,\"flags\":0}",buf);
}
static void test_nonfinite_values_are_null_on_every_axis() {
    for(float bad : {NAN, INFINITY, -INFINITY}) {
        TelemetryRecord r{};
        r.accel_x=r.accel_y=r.accel_z=r.gyro_x=r.gyro_y=r.gyro_z=r.window_rms=bad;
        char buf[MQTT_PAYLOAD_SIZE]; TEST_ASSERT_TRUE(buildPayload(r,buf,sizeof(buf)));
        TEST_ASSERT_NOT_NULL(strstr(buf,"\"ax\":null,\"ay\":null,\"az\":null,\"gx\":null,\"gy\":null,\"gz\":null,\"wrms\":null"));
        TEST_ASSERT_NULL(strstr(buf,"nan")); TEST_ASSERT_NULL(strstr(buf,"inf"));
    }
}
static void test_mixed_fields_preserve_valid_measurements() {
    TelemetryRecord r{}; r.accel_x=NAN; r.gyro_y=2.5f; r.window_rms=-INFINITY;
    char buf[MQTT_PAYLOAD_SIZE]; TEST_ASSERT_TRUE(buildPayload(r,buf,sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf,"\"ax\":null"));
    TEST_ASSERT_NOT_NULL(strstr(buf,"\"gy\":2.5000"));
    TEST_ASSERT_NOT_NULL(strstr(buf,"\"wrms\":null"));
}
static void test_extreme_finite_values_and_identity_fit() {
    TelemetryRecord r{}; r.boot_id=r.sequence_id=UINT32_MAX;
    r.timestamp_ms=UINT64_MAX; r.status_flags=UINT8_MAX;
    // Worst-case identity plus the longest retained fixed format must fit the real
    // firmware buffer; shrinking MQTT_PAYLOAD_SIZE below it must fail here.
    for(float value : {-FLT_MAX, FLT_MAX, -1e17f, 1e17f, FLT_MIN, -FLT_MIN}) {
        r.accel_x=r.accel_y=r.accel_z=r.gyro_x=r.gyro_y=r.gyro_z=r.window_rms=value;
        char buf[MQTT_PAYLOAD_SIZE]; TEST_ASSERT_TRUE(buildPayload(r,buf,sizeof(buf)));
        TEST_ASSERT_NULL(strstr(buf,"null")); TEST_ASSERT_NULL(strstr(buf,"inf"));
        TEST_ASSERT_EQUAL_CHAR('}',buf[strlen(buf)-1]);
    }
}
static void test_truncation_clears_output_and_exact_fit_succeeds() {
    TelemetryRecord r{}; char buf[MQTT_PAYLOAD_SIZE];
    TEST_ASSERT_TRUE(buildPayload(r,buf,sizeof(buf)));
    const size_t length=strlen(buf);
    TEST_ASSERT_TRUE(buildPayload(r,buf,length+1));
    TEST_ASSERT_FALSE(buildPayload(r,buf,length)); TEST_ASSERT_EQUAL_CHAR('\0',buf[0]);
    TEST_ASSERT_FALSE(buildPayload(r,buf,1)); TEST_ASSERT_EQUAL_CHAR('\0',buf[0]);
    TEST_ASSERT_FALSE(buildPayload(r,buf,0)); TEST_ASSERT_FALSE(buildPayload(r,nullptr,MQTT_PAYLOAD_SIZE));
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_finite_values_keep_wire_format);
    RUN_TEST(test_nonfinite_values_are_null_on_every_axis);
    RUN_TEST(test_mixed_fields_preserve_valid_measurements);
    RUN_TEST(test_extreme_finite_values_and_identity_fit);
    RUN_TEST(test_truncation_clears_output_and_exact_fit_succeeds);
    return UNITY_END();
}
