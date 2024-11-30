# ESP_BlueT_Temp_Conctol


<div style="display: flex; justify-content: space-around;">
  <img src="images/image-1.jpg" alt="Image1" style="width:200px;height:auto;">
  <img src="images/image-2.jpg" alt="Image2" style="width:200px;height:auto;">
  <img src="images/image-3.jpg" alt="Image3" style="width:200px;height:auto;">
</div>

这是一个基于 Flutter 框架开发的客户端对应的嵌入式代码，可以用于合宙的 ESP32-C3 ，来控制恒温箱的温控器，给其加上物联网功能，方便自动化温度控制。

本项目由学长的项目而来：https://github.com/LFF8888/TemperatureControl/ ，学长自己的代码的教程演示在 https://www.bilibili.com/video/BV1Kx421U7cX/

本项目基于 **MIT** 协议开源  


应用代码在：
https://github.com/SnowSwordScholar/Flutter_Bluetooth_Temperature_Control


### 注意，这个代码是两坨混起来的 Shift ，如果想看一坨的版本，请看 Master 分支的 *2024-11-29 代码正常* 提交。如果你想将代码应用到自己的设备，只需要修改 void executeSetting() , void setTemp(int a) , void setTempZero() 即可