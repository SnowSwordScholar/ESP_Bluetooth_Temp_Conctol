#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// 定义UUID
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcdefab-1234-5678-1234-abcdefabcdef"

// LED Pin
const int LED_PIN = 12; // D4 (IO12)

// EEPROM设置
const int MAX_TEMPERATURE_POINTS = 10;
const int BYTES_PER_POINT = 4; // 2字节时间，2字节温度
const int EEPROM_SIZE = MAX_TEMPERATURE_POINTS * BYTES_PER_POINT;

// 全局变量
BLECharacteristic *pCharacteristic;
bool deviceConnected = false;
bool isRunning = false;

// 前向声明任务函数
void blinkLEDTask(void * parameter);

// 创建BLE服务器回调
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
      deviceConnected = true;
      Serial.println("设备已连接");
    }

    void onDisconnect(BLEServer* pServer) override {
      deviceConnected = false;
      Serial.println("设备已断开连接");
      isRunning = false; // 停止运行
      digitalWrite(LED_PIN, LOW); // 确保LED关闭
    }
};

// 创建特征的回调
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) override {
      std::string rxValue = pCharacteristic->getValue();

      if (rxValue.length() > 0) {
        Serial.println("收到数据:");
        for (int i = 0; i < rxValue.length(); i++)
          Serial.print(rxValue[i]);
        Serial.println();

        // 限制接收数据大小
        if (rxValue.length() > 600) {
          Serial.println("接收到的数据过大，忽略");
          return;
        }

        // 解析JSON
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, rxValue);
        if (error) {
          Serial.print(F("JSON解析失败: "));
          Serial.println(error.f_str());
          return;
        }

        const char* command = doc["command"];
        if (strcmp(command, "get_temperature_points") == 0) {
          // 处理获取温控点的请求
          sendTemperaturePoints();
        } else if (strcmp(command, "set_temperature_points") == 0) {
          // 处理设置温控点的请求
          handleSetTemperaturePoints(doc["data"]);
        } else if (strcmp(command, "start_run") == 0) {
          // 处理开始运行的请求
          handleStartRun();
        } else if (strcmp(command, "interrupt") == 0) {
          // 处理中断运行的请求
          handleInterrupt();
        }
        // 处理其他命令
      }
    }

    void sendTemperaturePoints() {
      // 从EEPROM读取温控点数据
      DynamicJsonDocument response(2048);
      response["command"] = "temperature_points";
      JsonArray data = response.createNestedArray("data");

      Serial.println("读取EEPROM中的温控点数据:");
      for (int i = 0; i < MAX_TEMPERATURE_POINTS; i++) {
        int addr = i * BYTES_PER_POINT;
        byte timeLow = EEPROM.read(addr);
        byte timeHigh = EEPROM.read(addr + 1);
        byte tempLow = EEPROM.read(addr + 2);
        byte tempHigh = EEPROM.read(addr + 3);

        int time = (timeHigh << 8) | timeLow;
        int temperature = (tempHigh << 8) | tempLow;

        // 仅添加已设置的温控点（时间大于0）
        if (time > 0) {
          JsonObject point = data.createNestedObject();
          point["time"] = time;
          point["temperature"] = temperature;
          Serial.printf("温控点 %d - 时间: %d 分钟, 温度: %d°C\n", i + 1, time, temperature);
        }
      }

      String responseString;
      serializeJson(response, responseString);
      if (responseString.length() <= 600) {
        pCharacteristic->setValue(responseString.c_str());
        pCharacteristic->notify();
        Serial.println("已发送温控点数据");
      } else {
        Serial.println("响应数据过大，未发送");
      }
    }

    void handleSetTemperaturePoints(JsonArray data) {
      // 将温控点数据写入EEPROM
      Serial.println("设置温控点数据");
      int index = 0;
      for (JsonObject point : data) {
        if (index >= MAX_TEMPERATURE_POINTS) break;
        int time = point["time"];
        int temperature = point["temperature"];
        int addr = index * BYTES_PER_POINT;

        EEPROM.write(addr, time & 0xFF); // 时间低字节
        EEPROM.write(addr + 1, (time >> 8) & 0xFF); // 时间高字节
        EEPROM.write(addr + 2, temperature & 0xFF); // 温度低字节
        EEPROM.write(addr + 3, (temperature >> 8) & 0xFF); // 温度高字节

        Serial.printf("温控点 %d - 时间: %d 分钟, 温度: %d°C\n", index + 1, time, temperature);
        index++;
      }
      EEPROM.commit(); // 保存更改
      Serial.println("温控点数据已保存到EEPROM");

      // 发送验证结果
      DynamicJsonDocument response(256);
      response["command"] = "verify_temperature_points";
      response["status"] = "success";
      response["message"] = "温控点设置成功";

      String responseString;
      serializeJson(response, responseString);
      if (responseString.length() <= 600) {
        pCharacteristic->setValue(responseString.c_str());
        pCharacteristic->notify();
        Serial.println("温控点验证通过，已发送响应");
      } else {
        Serial.println("响应数据过大，未发送");
      }
    }

    void handleStartRun() {
      if (!isRunning) {
        isRunning = true;
        Serial.println("开始运行");
        sendRunStatus("started");
        // 启动LED闪烁任务
        xTaskCreatePinnedToCore(
          blinkLEDTask,         // 任务函数（自由函数）
          "BlinkLEDTask",      // 任务名称
          1024,                // 堆栈大小
          NULL,                // 任务参数
          1,                   // 优先级
          NULL,                // 任务句柄
          1                    // 运行在核心1
        );
      }
    }

    void handleInterrupt() {
      if (isRunning) {
        isRunning = false;
        Serial.println("运行已中断");
        sendRunStatus("interrupted");
        digitalWrite(LED_PIN, LOW); // 确保LED关闭
      }
    }

    void sendRunStatus(String status) {
      DynamicJsonDocument response(256);
      response["command"] = "run_status";
      response["status"] = status;
      response["message"] = "运行状态: " + status;

      String responseString;
      serializeJson(response, responseString);
      if (responseString.length() <= 600) {
        pCharacteristic->setValue(responseString.c_str());
        pCharacteristic->notify();
        Serial.println("已发送运行状态: " + status);
      } else {
        Serial.println("响应数据过大，未发送");
      }
    }

};

// LED闪烁任务函数（自由函数）
void blinkLEDTask(void * parameter){
  pinMode(LED_PIN, OUTPUT);
  Serial.println("LED闪烁任务已启动");
  while(isRunning){
    digitalWrite(LED_PIN, HIGH);
    vTaskDelay(500 / portTICK_PERIOD_MS); // LED亮0.5秒
    digitalWrite(LED_PIN, LOW);
    vTaskDelay(9500 / portTICK_PERIOD_MS); // LED灭9.5秒，总共每10秒闪烁一次
  }
  digitalWrite(LED_PIN, LOW); // 确保LED关闭
  Serial.println("LED闪烁任务已停止");
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("启动ing");

  // 初始化EEPROM
  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("EEPROM初始化失败!");
    while(1);
  }

  // 检查 EEPROM 是否已初始化（例如，检查第一个温控点时间是否为0或0xFFFF）
  int firstTime = EEPROM.read(0) | (EEPROM.read(1) << 8);
  if (firstTime == 0xFFFF || firstTime == 0x0000) { // 假设0xFFFF或0x0000表示未初始化
    Serial.println("EEPROM未初始化，进行初始化");
    for (int i = 0; i < MAX_TEMPERATURE_POINTS; i++) {
      int addr = i * BYTES_PER_POINT;
      EEPROM.write(addr, 0);       // 时间低字节
      EEPROM.write(addr + 1, 0);   // 时间高字节
      EEPROM.write(addr + 2, 0);   // 温度低字节
      EEPROM.write(addr + 3, 0);   // 温度高字节
    }
    EEPROM.commit();
    Serial.println("EEPROM已初始化");
  } else {
    Serial.println("EEPROM已包含温控点数据");
  }

  // 初始化LED引脚
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // 初始化BLE
  BLEDevice::init("ESP32_Temperature_Control"); // 确保名称与Flutter应用匹配
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_WRITE |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );

  pCharacteristic->addDescriptor(new BLE2902());
  pCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x06);  // 有助于iPhone连接
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("等待设备连接...");
}

void loop() {
  if (deviceConnected) {
    // 发送实时状态
    DynamicJsonDocument statusDoc(256);
    statusDoc["command"] = "current_status";
    statusDoc["data"]["runtime"] = 120; // 示例数据
    statusDoc["data"]["current_temperature"] = 25; // 示例数据

    String statusString;
    serializeJson(statusDoc, statusString);
    if (statusString.length() <= 600) {
      pCharacteristic->setValue(statusString.c_str());
      pCharacteristic->notify();
      Serial.println("已发送当前状态");
    } else {
      Serial.println("状态数据过大，未发送");
    }

    delay(30000); // 每30秒发送一次
  }

  delay(1000);
}