#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "cJSON.h"
#include "unity.h"

#include "parser.h"
#include "tmalloc.h"

static const char *TAG = "testing_parser";

void setUp() { };
void tearDown() { };

void test_HelloWorld(void) {
  TEST_IGNORE_MESSAGE("Hello world!");
}

void test_Blink()
{
  blink_t blink;
  
  const char jstr[] = "{"
                      "\"time\":68800,"
                      "\"mask\":\"ee0f\","
                      "\"code\":\"00112233445566778899aabbccddeeff\""
                      "}";
  const uint8_t mask[BLINK_MASK_SIZE] = {0xee, 0x0f};
  const uint8_t code[BLINK_CODE_SIZE] = {0x00, 0x11, 0x22, 0x33, 0x44,
                                         0x55, 0x66, 0x77, 0x88, 0x99,
                                         0xaa, 0xbb, 0xcc, 0xdd, 0xee};

  cJSON *obj = cJSON_Parse(jstr);
  TEST_ASSERT_NOT_NULL(obj);
  TEST_ASSERT_TRUE(parse_blink_object(obj, &blink));
  TEST_ASSERT_TRUE(blink.time == 68800);
  TEST_ASSERT_EQUAL_MEMORY(blink.mask, mask, BLINK_MASK_SIZE);
  TEST_ASSERT_EQUAL_MEMORY(blink.code, code, BLINK_CODE_SIZE);
  cJSON_Delete(obj);
}

void test_Track()
{
  track_t track;
  
  const char jstr[] = "{"
                      "\"name\":\"68da94e8526e2d669f77d07d65fd4845\","
                      "\"size\":1655788,"
                      "\"time\":400"
                      "}";

  const uint8_t name[MD5_SIZE] = {0x68, 0xda, 0x94, 0xe8, 0x52, 0x6e,
                                  0x2d, 0x66, 0x9f, 0x77, 0xd0, 0x7d,
                                  0x65, 0xfd, 0x48, 0x45};

  cJSON *obj = cJSON_Parse(jstr);
  TEST_ASSERT_NOT_NULL(obj);
  TEST_ASSERT_TRUE(parse_track_object(obj, &track));
  TEST_ASSERT_TRUE(track.size == 1655788);
  TEST_ASSERT_TRUE(track.time == 400);
  TEST_ASSERT_EQUAL_MEMORY(track.md5, name, MD5_SIZE);
}

void test_ParseEmptyGroupShouldFail_no_time()
{
  group_t group;
  const char jstr[] = "{}";
  cJSON *obj = cJSON_Parse(jstr);
  TEST_ASSERT_NOT_NULL(obj);
  TEST_ASSERT_FALSE(parse_group_object(obj, &group));
}

void test_ParseGroupWithOnlyTimeShouldSucceed()
{
  group_t group;
  const char jstr[] = "{\"time\":1234}";
  cJSON *obj = cJSON_Parse(jstr);
  TEST_ASSERT_NOT_NULL(obj);
  TEST_ASSERT_TRUE(parse_group_object(obj, &group));
  TEST_ASSERT_TRUE(group.time == 1234);
}

void test_ParseGroupWithEmptyChildrenShouldSucceed()
{
  group_t group;
  const char jstr[] = "{"
                      "\"children\":[],"
                      "\"time\":1234"
                      "}";

  cJSON *obj = cJSON_Parse(jstr);
  TEST_ASSERT_NOT_NULL(obj);
  TEST_ASSERT_TRUE(parse_group_object(obj, &group));
  TEST_ASSERT_TRUE(group.time == 1234);
  TEST_ASSERT_TRUE(group.children_array_size == 0);
  TEST_ASSERT_NULL(group.children);
}

void test_ParseGroupWithSingleValidChildShouldSucceed()
{
  group_t group;
  const char jstr[] = "{"
                      "\"children\":[{"
                      "\"time\":1234"
                      "}],"
                      "\"time\":1234"
                      "}";

  cJSON *obj = cJSON_Parse(jstr);
  TEST_ASSERT_NOT_NULL(obj);
  TEST_ASSERT_TRUE(parse_group_object(obj, &group));
  destroy_group_object(&group); 
  TEST_ASSERT_TRUE(tmalloc_no_leak());
}

void app_main(void) {
  ESP_LOGI(TAG, "testing json started");

  UNITY_BEGIN();
  RUN_TEST(test_HelloWorld);
  RUN_TEST(test_Blink);
  RUN_TEST(test_Track);
  RUN_TEST(test_ParseEmptyGroupShouldFail_no_time);
  RUN_TEST(test_ParseGroupWithOnlyTimeShouldSucceed);
  RUN_TEST(test_ParseGroupWithEmptyChildrenShouldSucceed);
  RUN_TEST(test_ParseGroupWithSingleValidChildShouldSucceed);

  UNITY_END();

  for (;;) {
    vTaskDelay(1000 * 1000 / portTICK_PERIOD_MS);
  }
}
