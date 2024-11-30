#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <cstring>

// 定义UUID
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcdefab-1234-5678-1234-abcdefabcdef"

// LED Pin
const int LED_PIN_D4 = 12; // D4 (IO12)
const int LED_PIN_D5 = 13;



// 定义 PWM 相关参数用于呼吸效果
const int LEDC_CHANNEL_D4 = 0;
const int LEDC_CHANNEL_D5 = 1;
const int LEDC_TIMER = 0;
const int LEDC_FREQ = 5000; // 5kHz
const int LEDC_RESOLUTION = 8; // 8-bit分辨率

// 定义 LED 状态
enum LEDState {
    WAITING_FOR_CONNECTION,
    CONNECTION_SUCCESS,
    RECEIVING_SUCCESS,
    EXECUTING,
    EXECUTING_WITHOUT_CONECT,
    COMPLETED
};

// 当前状态，初始为等待连接
volatile LEDState currentState = WAITING_FOR_CONNECTION;

// 用户按键设置
const int BOOT_PIN = 9; // 用户按键 (GPIO9)
unsigned long pressStartTime = 0; // 按下时的时间戳
bool isPressed = false;           // 按钮是否按下
bool longPressTriggered = false;  // 是否触发过长按功能


// EEPROM设置
const int MAX_TEMPERATURE_POINTS = 15;
const int BYTES_PER_POINT = 4; // 2字节时间，2字节温度
const int EEPROM_SIZE = MAX_TEMPERATURE_POINTS * BYTES_PER_POINT;
int addr = 0;

//温度段数组
int newData[MAX_TEMPERATURE_POINTS][2];  // 原始数据数组
int loadData[MAX_TEMPERATURE_POINTS][2]; // 用于加载数据的数组

// 设置按键
int KEY1 = 6;   //设定
int KEY2 = 10;  //减
int KEY3 = 3;   //加
int KEY4 = 2;   //左移

//计时相关变量
unsigned long startTime = 0; // 存储计时开始或重置的时间点
unsigned long currentTime = 0; // 存储当前时间
unsigned long lastPrintTime = 0; // 上一次打印时间
int currentEvent = 0; // 当前检查的事件索引

// 定义一个全局变量，用于跟踪上一次插值的时间
unsigned long lastInterpolationTime = 0;
// 每次插值间隔（秒）
const int interpolationInterval = 1;


// 轮询广播设置
unsigned long previousMillis = 0; // 保存上次发送状态的时间
const long interval = 5000; // 发送状态的时间间隔（毫秒）

// 轮询温度设置
unsigned long tempPreviousMillis = 0; // 保存上次发送状态的时间
const long tempInterval = 5000; // 更新状态的时间间隔（毫秒）

// 标志位设置
bool isStart = 0;    // 是否已经启动
bool isInterpolated = 0; // 当前是否被插值

// 全局变量
BLECharacteristic *pCharacteristic;
bool deviceConnected = false;
bool isRunning = false;
bool hasSentTemperaturePoints = false;
int NowTemp =0;  // 插入的值

// 前向声明任务函数
void ledTask(void * parameter);

// 全局函数声明
void sendTemperaturePoints();
void setTemp(int a);



// LFF 的屎山函数
// 执行设定值
void executeSetting() {
    // 从EEPROM读取数据
    int addr = 0;

    for (int i = 0; i < MAX_TEMPERATURE_POINTS; i++) {
        // 读取时间（2字节）
        byte timeLow = EEPROM.read(addr);
        byte timeHigh = EEPROM.read(addr + 1);
        int time = (timeHigh << 8) | timeLow;

        // 读取温度（2字节）
        byte tempLow = EEPROM.read(addr + 2);
        byte tempHigh = EEPROM.read(addr + 3);
        int temperature = (tempHigh << 8) | tempLow;

        // 更新地址到下一个温控点
        addr += BYTES_PER_POINT;

        // 检查时间是否有效并存储到 loadData
        if (time >= 0 && time < 1000) { // 假设时间在0到1000分钟之间有效
            loadData[i][0] = time;       // 第0列存储时间
            loadData[i][1] = temperature; // 第1列存储温度

            Serial.printf("温控点 %d - 时间: %d 分钟, 温度: %d°C\n", i + 1, time, temperature);
        } else {
            // 如果时间无效，标记为默认值或跳过
            loadData[i][0] = 114514; // 标记时间无效
            loadData[i][1] = 114514; // 标记温度无效

            Serial.printf("温控点 %d - 未设置或无效的数据，跳过\n", i + 1);
        }
        isStart = 1;
    }

    // 打印设定值数组
    Serial.println("\n设定值：");
    for (int i = 0; i < MAX_TEMPERATURE_POINTS; i++) {
        // 检查当前行是否已经是默认的0值，如果是，则不打印后续的数据
        if (loadData[i][0] == 114514 && loadData[i][1] == 114514) break;

        Serial.print("温度点");
        Serial.print(i + 1); // 显示温度点的编号，从1开始
        Serial.print("：");
        Serial.print(loadData[i][0]); // 打印分钟数
        Serial.print("分钟 ");
        Serial.print(loadData[i][1]); // 打印温度值
        Serial.println("度");
    }

    startTime = millis(); // 重置开始时间
    currentEvent = 0;
    isStart = 1;
}

// 重置设定值
void resetSetting() {
  byte value = 0xff;
  Serial.print("开始重置 EEPROM，设置所有字节为: 0x");
  Serial.println(value, HEX);
  
  for (int addr = 0; addr < EEPROM_SIZE; addr++) {
    EEPROM.write(addr, value);
  }
  
  // 确保数据写入 EEPROM
  if (EEPROM.commit()) {
    Serial.println("EEPROM 重置成功！");
  } else {
    Serial.println("EEPROM 重置失败！");
  }
}

void tempEvent() {
  if (currentEvent < MAX_TEMPERATURE_POINTS - 1 && loadData[currentEvent][0] != 114514) { // 确保至少还有一个后续事件
    unsigned long currentTime = (millis() - startTime) / 1000 / 60; // 计算当前时间（分钟）

    // 当时间达到当前事件指定的时间时
    if (currentTime >= loadData[currentEvent][0]) {
      // 设置初始温度
      setTemp(loadData[currentEvent][1]);
      Serial.print("温度已经设定为: ");
      Serial.println(loadData[currentEvent][1]);

      // 准备开始插值到下一个事件的温度
      currentEvent++;
      lastInterpolationTime = currentTime; // 重置插值时间
      // isInterpolated = 0;
    }
    else if (currentTime - lastInterpolationTime >= interpolationInterval && currentEvent > 0) {
      // 插值计算
      float timeDiff = loadData[currentEvent][0] - loadData[currentEvent - 1][0];
      if (timeDiff > 0) {
        float tempDiff = loadData[currentEvent][1] - loadData[currentEvent - 1][1];
        float fraction = (currentTime - loadData[currentEvent - 1][0]) / timeDiff;
        int interpolatedTemp = loadData[currentEvent - 1][1] + round(tempDiff * fraction);

        // 更新温度
        setTemp(interpolatedTemp);
        Serial.print("插值温度更新为: ");
        Serial.println(interpolatedTemp);
      }
      lastInterpolationTime = currentTime; // 更新插值时间
      // isInterpolated = 1;
    }
    currentState = EXECUTING;
  }else if (isStart){
    currentState = COMPLETED ;
  }
}


// 打印时间戳
void printTime() {
  currentTime = millis(); // 获取当前时间点

  if (currentTime - lastPrintTime >= 10000) { // 每隔至少1000毫秒更新一次

    lastPrintTime = currentTime; // 更新上一次打印时间
    
    unsigned long elapsed = currentTime - startTime; // 计算经过的时间
    unsigned long seconds = elapsed / 1000; // 总秒数
    unsigned long minutes = seconds / 60; // 总分钟数
    unsigned long hours = minutes / 60; // 总小时数
    unsigned long days = hours / 24; // 总天数

    // 计算剩余的小时、分钟和秒
    hours = hours % 24;
    minutes = minutes % 60;
    seconds = seconds % 60;

    // 打印当前运行时间
    Serial.print("程序已运行 ");
    Serial.print(days);
    Serial.print(" 天 ");
    Serial.print(hours);
    Serial.print(" 小时 ");
    Serial.print(minutes);
    Serial.print(" 分 ");
    Serial.print(seconds);
    Serial.println(" 秒");
    delay(10);
  }
}

// 设置温控器温度为0
void setTempZero(){
  // KEY1按一次
  digitalWrite(KEY1, LOW);delay(50);digitalWrite(KEY1, HIGH);delay(50);

  // KEY4按3次
  for (int i = 0; i < 3; i++) {
    digitalWrite(KEY4, LOW);delay(25);digitalWrite(KEY4, HIGH);delay(25);
  }

  // KEY2按2次
  for (int i = 0; i < 2; i++) {
    digitalWrite(KEY2, LOW);delay(25);digitalWrite(KEY2, HIGH);delay(25);
  }

  // KEY1按一次
  digitalWrite(KEY1, LOW);delay(50);digitalWrite(KEY1, HIGH);delay(50);
}

// 设置温控器温度
void setTemp(int a) {
  NowTemp = a;
  setTempZero();    // 设置温度为零

  // KEY1按一次,表示启动
  digitalWrite(KEY1, LOW);delay(50);digitalWrite(KEY1, HIGH);delay(50);

  // 获取三位数的个位数
  int units = a % 10;
  // 让KEY3闪烁units次
  for (int i = 0; i < units; i++) {
    digitalWrite(KEY3, LOW);delay(25);digitalWrite(KEY3, HIGH);delay(25);
  }

  // KEY4按一次,表示前移一位
  digitalWrite(KEY4, LOW);delay(25);digitalWrite(KEY4, HIGH);delay(25);

  // 获取三位数的十位数
  int tens = (a / 10) % 10;
  // 让KEY3闪烁tens次
  for (int i = 0; i < tens; i++) {
    digitalWrite(KEY3, LOW);delay(25);digitalWrite(KEY3, HIGH);delay(25);
  }

  // KEY4按一次,表示前移一位
  digitalWrite(KEY4, LOW);delay(25);digitalWrite(KEY4, HIGH);delay(25);

  // 获取三位数的百位数
  int hundreds = a / 100;
  // 让KEY3闪烁hundreds次
  for (int i = 0; i < hundreds; i++) {
    digitalWrite(KEY3, LOW);delay(25);digitalWrite(KEY3, HIGH);delay(25);
  }

  // KEY1按一次,表示结束
  digitalWrite(KEY1, LOW);delay(50);digitalWrite(KEY1, HIGH);delay(50);
}







// 创建BLE服务器回调
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
      deviceConnected = true;
      Serial.println("设备已连接");
      if(isStart){
        currentState = EXECUTING;
      }else{
        currentState = CONNECTION_SUCCESS;
      }
      
      hasSentTemperaturePoints = false; // 重置标志位，以便发送温控点
    }

    void onDisconnect(BLEServer* pServer) override {
      deviceConnected = false;
      Serial.println("设备已断开连接");
      // isRunning = false; // 停止运行
      if(isRunning){
        currentState = EXECUTING_WITHOUT_CONECT;
      }else{
        currentState = WAITING_FOR_CONNECTION;
      }
      
      // 重新启动广告
      BLEDevice::startAdvertising();
      Serial.println("重新开始广告，等待设备连接...");
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

        // // 限制接收数据大小
        // if (rxValue.length() > 600) {
        //   Serial.println("接收到的数据过大，忽略");
        //   return;
        // }

        // 解析JSON
        DynamicJsonDocument doc(20480);
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

    void handleSetTemperaturePoints(JsonArray data) {
      // 将温控点数据写入EEPROM
      Serial.println("设置温控点数据");
      int index = 0;

      // 遍历 JSON 数据并写入温控点
      for (JsonObject point : data) {
          if (index >= MAX_TEMPERATURE_POINTS) break;
          int time = point["time"];
          int temperature = point["temperature"];
          int addr = index * BYTES_PER_POINT;

          EEPROM.write(addr, time & 0xFF);            // 时间低字节
          EEPROM.write(addr + 1, (time >> 8) & 0xFF); // 时间高字节
          EEPROM.write(addr + 2, temperature & 0xFF); // 温度低字节
          EEPROM.write(addr + 3, (temperature >> 8) & 0xFF); // 温度高字节

          Serial.printf("温控点 %d - 时间: %d 分钟, 温度: %d°C\n", index + 1, time, temperature);
          index++;
      }

      // 填充剩余的区域为 0xFF
      for (int i = index; i < MAX_TEMPERATURE_POINTS; i++) {
          int addr = i * BYTES_PER_POINT;
          for (int j = 0; j < BYTES_PER_POINT; j++) {
              EEPROM.write(addr + j, 0xFF);
          }
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
        delay(300); // 确保客户端有足够时间处理数据
        pCharacteristic->setValue(responseString.c_str());
        pCharacteristic->notify();
        Serial.println("温控点验证通过，已发送响应");
      } else {
        Serial.println("响应数据过大，未发送");
        return;
      }
      isStart = 0;
      currentState = RECEIVING_SUCCESS;
    }

    void handleStartRun() {
      if (!isRunning) {
        isRunning = true;
        Serial.println("开始运行");
        sendRunStatus("started");
        // 启动LED闪烁任务
        currentState = EXECUTING;
      }
      executeSetting();
    }

    void handleInterrupt() {
      if (isRunning) {
        isRunning = false;
        Serial.println("运行已中断");
        sendRunStatus("interrupted");
        currentState = CONNECTION_SUCCESS;   //  LED 
        isStart = 0;
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
        delay(300); // 确保客户端有足够时间处理数据
        pCharacteristic->setValue(responseString.c_str());
        pCharacteristic->notify();
        Serial.println("已发送运行状态: " + status);
      } else {
        Serial.println("响应数据过大，未发送");
      }
    }
};

// 全局函数定义
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

        // 排除时间为0或65535的温控点
        if (time >= 0 && time < 1000) { // 假设时间不会超过1000分钟
            JsonObject point = data.createNestedObject();
            point["time"] = time;
            point["temperature"] = temperature;
            Serial.printf("温控点 %d - 时间: %d 分钟, 温度: %d°C\n", i + 1, time, temperature);
        } else {
            Serial.printf("温控点 %d - 未设置或无效的数据，跳过\n", i + 1);
        }
    }

    String responseString;
    serializeJson(response, responseString);
    if (responseString.length() <= 600) {
        delay(777); // 确保客户端有足够时间处理数据
        pCharacteristic->setValue(responseString.c_str());
        pCharacteristic->notify();
        Serial.println("已发送温控点数据");
    } else {
        Serial.println("响应数据过大，未发送");
    }
}

// LED 控制任务
void ledTask(void * parameter) {
    while (1) {
        switch(currentState) {
            case WAITING_FOR_CONNECTION:
                // 两颗 LED 交替闪烁
                ledcWrite(LEDC_CHANNEL_D4, 255); // D4 打开
                ledcWrite(LEDC_CHANNEL_D5, 0);   // D5 关闭
                vTaskDelay(500 / portTICK_PERIOD_MS);
                ledcWrite(LEDC_CHANNEL_D4, 0);   // D4 关闭
                ledcWrite(LEDC_CHANNEL_D5, 255); // D5 打开
                vTaskDelay(500 / portTICK_PERIOD_MS);
                break;
                
            case CONNECTION_SUCCESS:
                // 两颗 LED 常亮
                ledcWrite(LEDC_CHANNEL_D4, 255); // D4 常亮
                ledcWrite(LEDC_CHANNEL_D5, 255); // D5 常亮
                vTaskDelay(1000 / portTICK_PERIOD_MS); // 延时避免占用过多 CPU
                break;
                
            case RECEIVING_SUCCESS:
                // D5 快速闪烁，D4 关闭
                ledcWrite(LEDC_CHANNEL_D4, 0);   // D4 关闭
                ledcWrite(LEDC_CHANNEL_D5, 255); // D5 打开
                vTaskDelay(200 / portTICK_PERIOD_MS);
                ledcWrite(LEDC_CHANNEL_D5, 0);   // D5 关闭
                vTaskDelay(200 / portTICK_PERIOD_MS);
                break;
                
            case EXECUTING:
                // 两颗 LED 交替呼吸
                for(int brightness = 0; brightness < 200; brightness++) {
                    ledcWrite(LEDC_CHANNEL_D4, brightness);
                    ledcWrite(LEDC_CHANNEL_D5, 200 - brightness);
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                    // 如果状态改变，跳出循环
                    if(currentState != EXECUTING) break;
                }
                for(int brightness = 200; brightness >= 0; brightness--) {
                    ledcWrite(LEDC_CHANNEL_D4, brightness);
                    ledcWrite(LEDC_CHANNEL_D5, 200 - brightness);
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                    // 如果状态改变，跳出循环
                    if(currentState != EXECUTING) break;
                }
                break;
            case EXECUTING_WITHOUT_CONECT:
                ledcWrite(LEDC_CHANNEL_D4, 0);
                for(int brightness = 0; brightness < 200; brightness++) {
                    ledcWrite(LEDC_CHANNEL_D5, 200 - brightness);
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                    // 如果状态改变，跳出循环
                    if(currentState != EXECUTING_WITHOUT_CONECT) break;
                }
                for(int brightness = 200; brightness >= 0; brightness--) {
                    ledcWrite(LEDC_CHANNEL_D5, 200 - brightness);
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                    // 如果状态改变，跳出循环
                    if(currentState != EXECUTING_WITHOUT_CONECT) break;
                }
                break;
            case COMPLETED:
                // D5 快速闪烁，D4 关闭
                ledcWrite(LEDC_CHANNEL_D4, 255); 
                ledcWrite(LEDC_CHANNEL_D5, 0);
                vTaskDelay(200 / portTICK_PERIOD_MS);
                ledcWrite(LEDC_CHANNEL_D4, 0); 
                vTaskDelay(200 / portTICK_PERIOD_MS);
                break;
        }
    }
}
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("启动ing");
    // delay(5000);  //等待温控器启动
    // 初始化EEPROM
    EEPROM.begin(EEPROM_SIZE);

    // 打印EEPROM原始值
    Serial.println("EEPROM原始数据:");
    for (int i = 0; i < EEPROM_SIZE; i++) {
        Serial.printf("0x%02X ", EEPROM.read(i));
        if ((i + 1) % BYTES_PER_POINT == 0) Serial.println();
    }

    // 打印EEPROM解析后的温控点数据
    Serial.println("EEPROM解析后的温控点数据:");
    for (int i = 0; i < MAX_TEMPERATURE_POINTS; i++) {
        int addr = i * BYTES_PER_POINT;
        byte timeLow = EEPROM.read(addr);
        byte timeHigh = EEPROM.read(addr + 1);
        byte tempLow = EEPROM.read(addr + 2);
        byte tempHigh = EEPROM.read(addr + 3);

        int time = (timeHigh << 8) | timeLow;
        int temperature = (tempHigh << 8) | tempLow;

        // 排除时间为0或65535的温控点
        if (time >= 0 && time < 1000) { // 假设时间不会超过1000分钟
            Serial.printf("温控点 %d - 时间: %d 分钟, 温度: %d°C\n", i + 1, time, temperature);
        } else {
            Serial.printf("温控点 %d - 未设置或无效的数据，跳过\n", i + 1);
        }
    }

    // 可选：检查 EEPROM 是否已初始化（例如，检查第一个温控点时间是否为0或0xFFFF）
    int firstTime = EEPROM.read(0) | (EEPROM.read(1) << 8);
    if (firstTime == 0xFFFF) { // 假设0xFFFF或0x0000表示未初始化
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

    // 初始化 LED 引脚
    pinMode(LED_PIN_D4, OUTPUT);
    pinMode(LED_PIN_D5, OUTPUT);

    pinMode(BOOT_PIN, INPUT_PULLUP); // 设置按键为输入模式，使用内部上拉电阻

    // 设置 PWM 通道用于呼吸效果
    ledcSetup(LEDC_CHANNEL_D4, LEDC_FREQ, LEDC_RESOLUTION);
    ledcAttachPin(LED_PIN_D4, LEDC_CHANNEL_D4);
    
    ledcSetup(LEDC_CHANNEL_D5, LEDC_FREQ, LEDC_RESOLUTION);
    ledcAttachPin(LED_PIN_D5, LEDC_CHANNEL_D5);
    
    // 创建 LED 控制任务
    xTaskCreate(
        ledTask,            // Task 函数
        "LED Task",        // Task 名称
        2048,              // Task 栈大小
        NULL,              // Task 参数
        1,                 // Task 优先级
        NULL               // Task 句柄
    );

    // 初始化引脚
    pinMode(KEY1, OUTPUT);
    pinMode(KEY2, OUTPUT);
    pinMode(KEY3, OUTPUT);
    pinMode(KEY4, OUTPUT);
    // 关闭所有按键
    digitalWrite(KEY1, HIGH);
    digitalWrite(KEY2, HIGH);
    digitalWrite(KEY3, HIGH);
    digitalWrite(KEY4, HIGH);


    startTime = millis();   // 重置开始时间


    // 初始化BLE
    BLEDevice::init("ESP32_Temperature_Controll"); // 确保名称与Flutter应用匹配
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
    if (deviceConnected && !hasSentTemperaturePoints) {
        sendTemperaturePoints();
        hasSentTemperaturePoints = true;
    }

  if (deviceConnected) {
    // 发送实时状态
    unsigned long currentMillis = millis(); // 获取当前时间（毫秒）

    if (currentMillis - previousMillis >= interval) {
      // 检查是否到了发送状态的时间
      DynamicJsonDocument statusDoc(256);
      statusDoc["command"] = "current_status";
      statusDoc["data"]["runtime"] = isStart ? (millis() - startTime) / 60000 : 0; // 示例数据
      statusDoc["data"]["current_temperature"] = isStart ? NowTemp : 0 ; // 示例数据

      String statusString;
      serializeJson(statusDoc, statusString);
      if (statusString.length() <= 600) {
        pCharacteristic->setValue(statusString.c_str());
        pCharacteristic->notify();
        Serial.println("已发送当前状态");
      } else {
        Serial.println("状态数据过大，未发送");
      }

      previousMillis = currentMillis; // 更新上次发送状态的时间
    }
  }

  if (isStart && (millis()-tempPreviousMillis >= tempInterval)){
      printTime();  // 打印时间
      tempEvent();  // 处理温度事件
    tempPreviousMillis = millis();
  }

  // 按钮事件
    int buttonState = digitalRead(BOOT_PIN); // 读取按键状态

  // 如果按钮被按下
  if (buttonState == LOW) {
    if (!isPressed) {
      pressStartTime = millis();  // 记录按下时间
      isPressed = true;           // 标记为已按下
    }

    // 检查是否按下超过10秒
    if (isPressed && millis() - pressStartTime > 10000) {
      resetSetting();
      Serial.printf("清除数据，即将重启");
      currentState = CONNECTION_SUCCESS;
      delay(3000);
      esp_restart();
    }

    // 检查是否按下超过3秒但小于10秒
    if (isPressed && !longPressTriggered && millis() - pressStartTime > 3000) {
      currentState = RECEIVING_SUCCESS;
      longPressTriggered = true; // 标记为已触发长按功能
    }
  } else {
    // 如果按钮释放
    if (isPressed) {
      if (millis() - pressStartTime > 3000 && millis() - pressStartTime <= 10000) {
        Serial.printf("即将执行设定值\n");
        if(deviceConnected){
          currentState = EXECUTING;
        }else{
          currentState = EXECUTING_WITHOUT_CONECT;
        }
        executeSetting();
      }
      isPressed = false;          // 重置按下状态
      longPressTriggered = false; // 重置长按标记
    }
  }
}