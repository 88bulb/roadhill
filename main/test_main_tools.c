#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "unity.h"

#include "tools.h"

static const char *TAG = "testing_tools";

void setUp() {};
void tearDown() {};

void test_HelloWorld(void) {
  TEST_IGNORE_MESSAGE("Hello world!");
}

void test_HexStringToBytes_0xEE0F_uppercase(){
  const char* cstr = "EE0F";
  const int maxlen = 2; 
  uint8_t buf[maxlen]; 
  int len;

  TEST_ASSERT_TRUE(hex_string_to_bytes(cstr, buf, &len, maxlen));
  TEST_ASSERT_TRUE(buf[0] == 0xee);
  TEST_ASSERT_TRUE(buf[1] == 0x0f);
}

void test_HexStringToBytes_0xee0f_lowercase(){
  const char* cstr = "ee0f";
  const int maxlen = 2;
  uint8_t buf[maxlen]; 
  int len;

  TEST_ASSERT_TRUE(hex_string_to_bytes(cstr, buf, &len, maxlen));
  TEST_ASSERT_TRUE(buf[0] == 0xee);
  TEST_ASSERT_TRUE(buf[1] == 0x0f);
}

void test_HexStringToBytes_0xeexx_should_fail(){
  const char* cstr = "eexx";
  const int maxlen = 2;
  uint8_t buf[maxlen]; 
  int len;

  TEST_ASSERT_FALSE(hex_string_to_bytes(cstr, buf, &len, maxlen));
}

void app_main(void) {
  ESP_LOGI(TAG, "testing tools started"); 

  UNITY_BEGIN();
  RUN_TEST(test_HelloWorld);
  RUN_TEST(test_HexStringToBytes_0xEE0F_uppercase);
  RUN_TEST(test_HexStringToBytes_0xee0f_lowercase);
  RUN_TEST(test_HexStringToBytes_0xeexx_should_fail);
  UNITY_END();

  for (;;) {
    vTaskDelay(1000 * 1000 / portTICK_PERIOD_MS);
  }
}
