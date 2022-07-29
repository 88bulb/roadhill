#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "cJSON.h"
#include "unity.h"

#include "parser.h"

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
                      "\"mask\":\"ffff\","
                      "\"code\":\"100200000000000000000000000000\""
                      "}";
  cJSON *obj = cJSON_Parse(jstr);
  TEST_ASSERT_NOT_NULL(obj)
  TEST_ASSERT_TRUE(parse_blink_object(obj, &blink));
  cJSON_Delete(obj);
}

void app_main(void) {
  ESP_LOGI(TAG, "testing json parser started");

  UNITY_BEGIN();
  RUN_TEST(test_HelloWorld);

  ESP_LOGI(TAG, "testing json parser working in progress");

  RUN_TEST(test_Blink);
  UNITY_END();

  for (;;) {
    vTaskDelay(1000 * 1000 / portTICK_PERIOD_MS);
  }
}
