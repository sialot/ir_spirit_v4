// 遥控精灵
// v1 基础功能实现
// v2 添加指令断电存储功能
// v3 修复接收指令间隔不能小于200毫秒，发射指令间隔不能小于200毫秒的问题
//    板载led灯状态展示
// v4 完善界面,添加操作提示，修复任何状态都能进入录制的问题
// v4.5 修复录制时间遇到重复码，计算间隔不准确的问题
#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <U8g2lib.h>
#include <EEPROM.h>

///-----------------------------------------------------------------------------
// 系统状态及全局变量
///-----------------------------------------------------------------------------

#define MODE_PAUSING 0 // 等待播放
#define MODE_PLAYING 1 // 开始播放
#define MODE_RECORD_WAITING 2 // 录制
#define MODE_RECORDING 3 // 录制

#define BTN_PIN 0 // 模式切换按键针脚  D3
#define RECV_PIN 14 // 接收红外针脚      D7
#define SEND_PIN 13 // 发射红外信号针脚    D1 

#define RECORD_WAIT 1000 // 长按判定时间

#define LED_FLASH_TIME 3 // led闪光时间

int SYS_MODE = MODE_PAUSING;// 当前工作模式
bool screenFresh = false;// 显示刷新标志

///-----------------------------------------------------------------------------
// 显示屏相关
///-----------------------------------------------------------------------------

// 显示屏 ssd1306驱动  分辨率  未知厂商  全页缓存  软件模拟I2c   不转向，  时钟 SCL 数据SDA 无复位键
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock= D6 */ SCL  , /* data= D5 */ SDA , /* reset=*/ U8X8_PIN_NONE); 

///-----------------------------------------------------------------------------
// 红外相关
///-----------------------------------------------------------------------------
IRrecv irrecv(RECV_PIN);// 配置接收针脚
IRsend irsend(SEND_PIN);// 配置发射针脚
decode_results results;// 接收数据暂存

uint64_t last_ir_code = 0xCD123456;// 上一次的红外指令

typedef struct {
  unsigned long ptime;   // 与上一个指令的间隔时间
  uint64_t code; // 红外指令
} cmd_record;  // 命令记录


#define MAX_CMD_SIZE 100 // 最大存储个数
cmd_record CMD_ARR[MAX_CMD_SIZE];// 指令存储数组
unsigned long lastRecordTime = 0; // 上一条记录开始的时间

///-----------------------------------------------------------------------------
// 播放器
///-----------------------------------------------------------------------------
int cmdIdx = -1;  // 当前指令位置
int cmdSize = 0; // 存储指令个数
bool isAllowSendNexCode = false; // 允许发送下一个
#define LOOP_WAITING_TIME 2000 // 循环之间等待 n秒才开始；
unsigned long lastSendTime = 0; // 记录录制最开始的时间
bool isFirstTimePlay = true;
///-----------------------------------------------------------------------------
// 按钮事件
///-----------------------------------------------------------------------------
#define KEY_WAIT 0  // 无
#define KEY_CLICK 1 // 单击 
#define KEY_LT 2 // 长按
bool iskeyDown=false;
int keyDownTime = 0, keyUpTime = 0;// 按键时间记录
int keyCmd=0;// 按键指令  0 无  1 单击 2 长按

ICACHE_RAM_ATTR void keyDown(){
  iskeyDown = true;
  keyDownTime = millis();
  
  //Serial.println("↓");
  //Serial.println(keyDownTime);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), keyUp, RISING);
}
ICACHE_RAM_ATTR void keyUp(){
  iskeyDown = false;
  keyUpTime = millis();

  // millis未溢出，并且抬起时间小于长按等待时间，则为单击
  if(keyUpTime > keyDownTime && (keyUpTime - keyDownTime) < RECORD_WAIT){
    keyCmd = KEY_CLICK;
  }

  //Serial.println("↑");
  //Serial.println(keyUpTime);  
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), keyDown, FALLING);
}

// 按键回调函数
void keyCommondCallback(){

  // 按键中
  if(iskeyDown){

    // 只有暂停时，能进入录制
    if(SYS_MODE == MODE_PLAYING){
      return; 
    }else if(SYS_MODE == MODE_RECORDING || SYS_MODE == MODE_RECORD_WAITING){
      return;         
    }

    if((millis() - keyDownTime) > RECORD_WAIT){
      keyCmd = KEY_LT;
    }
  }

  if(keyCmd == KEY_WAIT){
    return;
  }

  if(keyCmd == KEY_CLICK){
   Serial.println("KEY_CLICK");
    if(SYS_MODE == MODE_PAUSING){
      play();
    }else if(SYS_MODE == MODE_PLAYING){
      stop(); 
    }else if(SYS_MODE == MODE_RECORDING || SYS_MODE == MODE_RECORD_WAITING){
      stopRecording();           
    }
    keyCmd = KEY_WAIT;
    return;
  }

  if(keyCmd == KEY_LT){


    Serial.println("KEY_LT");
    startRecord(); 
    keyCmd = KEY_WAIT;
    return;
  } 
}

///-----------------------------------------------------------------------------
// 播放
///-----------------------------------------------------------------------------
void playerInit(){

  if(isFirstTimePlay){
    cmdIdx = -1;    
  }

  SYS_MODE=MODE_PAUSING;

  // 启播获取下个编码
  isAllowSendNexCode = true;

  // 亮灯
  digitalWrite(LED_BUILTIN, LOW);
}
void play(){
  Serial.println("play");

  Serial.println("cmdIdx");
  Serial.println(cmdIdx);
  Serial.println("cmdSize");
  Serial.println(cmdSize);

  // 把当前时间当作上次发送信号的时间，用作暂停 或 首次播放的时间起始点
  lastSendTime = millis();

  // 无数据不可以播放
  if(cmdSize==0){
    return;
  }

  // 切换模式
  SYS_MODE=MODE_PLAYING;

  // 不再是第一次播放
  isFirstTimePlay = false;

  // 灭灯
  digitalWrite(LED_BUILTIN, HIGH);
  freshSrcNow();
}
void stop(){
  Serial.println("stop");
  SYS_MODE=MODE_PAUSING;

  // 亮灯
  digitalWrite(LED_BUILTIN, LOW);
  freshSrcNow();
}
bool isPlayDataUpdated = false;
void playLoop(){

  // 其他模式不处理
  if(SYS_MODE != MODE_PLAYING){
    return;
  }

  // 计时器溢出情况，本次放弃发送
  if(millis() < lastSendTime){
    lastSendTime = millis();
    return;
  }
 
  // 预测下一个
  int nextIdx = 0;
  if(cmdIdx < (cmdSize - 1)){
    nextIdx = cmdIdx+1;
  }else{

    // 循环延迟
    CMD_ARR[0].ptime = LOOP_WAITING_TIME;
  } 

  // 下一个间隔大于显示刷新时间，可以有时间刷新显示
  if(isPlayDataUpdated && CMD_ARR[nextIdx].ptime > 200){
    freshSrcNow();// 耗时 200 毫秒
    isPlayDataUpdated = false;
  }

  // 计算当前时间与上次发送时间间隔
  int duration = millis() - lastSendTime;

  // 到了下一个命令执行时间，切换指针，发送红外指令
  if(duration >= CMD_ARR[nextIdx].ptime){    
    cmdIdx = nextIdx;
    lastSendTime = millis(); 

    Serial.print("SEND> ");
    Serial.print(cmdIdx);
    Serial.print(" next ptime ");
    Serial.print(CMD_ARR[nextIdx].ptime);
    Serial.print(" duration ");
    Serial.print(duration);
    Serial.print(" ");
    Serial.println(toUpperCase(String(CMD_ARR[cmdIdx].code, HEX)));   

    digitalWrite(LED_BUILTIN, LOW);
    delay(LED_FLASH_TIME);
    digitalWrite(LED_BUILTIN, HIGH);   

    irsend.sendNEC(CMD_ARR[cmdIdx].code, 32); 

    // 标记数据刷新，在下一循环，刷新显示
    isPlayDataUpdated = true;
  }
}

///-----------------------------------------------------------------------------
// 录制
///-----------------------------------------------------------------------------
void startRecord(){
  Serial.println("startRecord");

  // 切换模式
  SYS_MODE=MODE_RECORD_WAITING;

  // 灭灯
  digitalWrite(LED_BUILTIN, HIGH);
  freshSrcNow();
}
void stopRecording(){
  Serial.println("stopRecording");
  saveCmd();
  playerInit();
  freshSrcNow();
}

// 录制监听
void recordLoop(){

  // 其他模式不处理
  if(SYS_MODE == MODE_PLAYING || SYS_MODE == MODE_PAUSING){
    return;
  }

  // 收到信号
  if(irrecv.decode(&results)){

    // 只解析nec
    if(results.decode_type == NEC){

      // 指令满了
      if(cmdSize == MAX_CMD_SIZE && SYS_MODE != MODE_RECORD_WAITING){
        freshSrcNow();
        irrecv.resume(); 
        return;
      }

      digitalWrite(LED_BUILTIN, LOW);
      delay(LED_FLASH_TIME);
      digitalWrite(LED_BUILTIN, HIGH);

      Serial.print(" RECV START millis() ");
      Serial.println(millis());

      if(!results.repeat){
    
        last_ir_code = results.value; // 暂存最新指令  
      } else {
        Serial.print(" repeat value ");
        Serial.println(results.value);
      }

      // 从收到第一个信号开始，正式开始录制
      if(SYS_MODE == MODE_RECORD_WAITING){

        // 重置指令位置
        cmdIdx=0; 

        // 存储个数归零
        cmdSize=0;

        // 以后从头播放
        isFirstTimePlay = true;
        
        // 清空记录
        memset(CMD_ARR, 0, sizeof(CMD_ARR));

        // 切换系统状态
        SYS_MODE=MODE_RECORDING;

        // 记录上次发送时间 = 收到时间 - 传输时间
        lastRecordTime=millis() - 110;
      }
       
      // 新指令
      cmd_record c;

      // 时间  
      if(!results.repeat){
        
        // 收到时间 - 传输时间 - 上次发送时间
        c.ptime = millis() - 110 - lastRecordTime;

        // 本次传输起始时间
        lastRecordTime=millis() - 110;   
      } else {

        //收到时间 - 传输时间 - 上次发送时间
        c.ptime = millis() - 50 - lastRecordTime; 

        // 本次传输起始时间
        lastRecordTime=millis() - 50;
      }
     
      // 指令code
      c.code = last_ir_code;

      // 数组赋值
      CMD_ARR[cmdIdx]=c;

      // 移动指针
      cmdIdx++;

      // 计数
      cmdSize++;
      
      Serial.print(">>>>>>>>>>>>>>>>>>>>>>>>>>>>recording time:");
      Serial.print(c.ptime);
      Serial.print(" code:");
      Serial.print(c.code);
      Serial.print(" ");
      Serial.print(" cmdIdx ");
      Serial.print(cmdIdx);
      Serial.print(" millis() ");
      Serial.print(millis());
      Serial.print(" results.repeat ");
        Serial.println(results.repeat);
    } 
    irrecv.resume();  
  }
}

///-----------------------------------------------------------------------------
// 显示
///-----------------------------------------------------------------------------

#define CN_1st_LINE_X 0
#define CN_4th_LINE_X 20
#define TXT_1st_LINE_Y 14
#define TXT_2nd_LINE_Y 31
#define TXT_3rh_LINE_Y 47
#define TXT_4th_LINE_Y 63

void freshScr(){
  screenFresh = true;
}
void freshSrcNow(){
  screenFresh = true;
  infoDisplay();
}

// 展示指令数据
void cmdDataDis(){
  
  // 基准线
  //u8g2.drawLine(0,16,128,16);
  // u8g2.drawLine(0,32,128,32);
  // u8g2.drawLine(0,48,128,48);
  // u8g2.drawLine(0,64,128,64);

  // 第一行  1/100 进度
  u8g2.setFont(u8g2_font_t0_13_tf);
  int x = 78;
  if(SYS_MODE!=MODE_RECORD_WAITING){
    
    if(SYS_MODE == MODE_RECORDING){      
      u8g2.setCursor(_getX(x, cmdSize, MAX_CMD_SIZE), TXT_1st_LINE_Y);

      u8g2.print(cmdSize);
      u8g2.print("/");
      u8g2.print(MAX_CMD_SIZE);
    }else{

      u8g2.setCursor(_getX(x, (cmdIdx+1), cmdSize), TXT_1st_LINE_Y);
      u8g2.print(cmdIdx+1); // 数组下标加1，提高可读性
      u8g2.print("/");
      u8g2.print(cmdSize);
    }
  } 

  // 滚动设置
  int disIdx =0;
  bool curP=true;
  if(SYS_MODE == MODE_RECORDING){

    if(cmdIdx == 1){
      disIdx = cmdIdx-1;
      curP=true;
    }else if(cmdIdx>1){
      disIdx = cmdIdx-2;
      curP=false;
    }
  }else if(cmdIdx == -1){
    disIdx = cmdIdx + 1;
    curP=true;
  }else{
    disIdx = cmdIdx;
    curP=true;
  }

  // 第二行  指令
  u8g2.setFont(u8g2_font_t0_13_tf);
  
  x = 0;
  if((disIdx+1)<10){
    x=x+u8g2.getMaxCharWidth();
  }
  if((disIdx+1)<100){
    x=x+u8g2.getMaxCharWidth();
  }
  u8g2.setCursor(x,TXT_2nd_LINE_Y);
  u8g2.print(disIdx + 1);

  u8g2.setCursor(32,TXT_2nd_LINE_Y);
  if(CMD_ARR[disIdx].code == 0){
    u8g2.print("__________");
  }else{

    // 拼接16进制码值
    String str = "0x" + toUpperCase(String(CMD_ARR[disIdx].code, HEX));
    u8g2.print(str); 
  }
  
  if(cmdIdx != -1){
    if(curP){
      u8g2.print(" <");
    }
  }
  
  // 第三行  指令
  x = 0;

  if((disIdx+1)<MAX_CMD_SIZE){
    if((disIdx+2)<10){
      x=x+u8g2.getMaxCharWidth();
    }
    if((disIdx+2)<100){
      x=x+u8g2.getMaxCharWidth();
    }
    u8g2.setCursor(x,TXT_3rh_LINE_Y);
    u8g2.print(disIdx + 2);

    u8g2.setCursor(32,TXT_3rh_LINE_Y);
    if(CMD_ARR[disIdx+1].code == 0){
      u8g2.print("__________");
    }else{

      // 拼接16进制码值
      String str = "0x" + toUpperCase(String(CMD_ARR[disIdx+1].code, HEX));
      u8g2.print(str); 
    }

    if(cmdIdx != -1){
      if(!curP){
        u8g2.print(" <");
      }
    }
  }
}


// 显示刷新
void infoDisplay() {
  if(!screenFresh){
    return;
  }

  u8g2.clearBuffer();


  // 第一行  pausing 1/100
  u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
  u8g2.setCursor(CN_1st_LINE_X,TXT_1st_LINE_Y);

  switch(SYS_MODE){

    case MODE_PAUSING:
      u8g2.print("IPTV 遥控精灵");
      cmdDataDis();

      // 操作提示
      u8g2.setFont(u8g2_font_wqy12_t_gb2312a);  
      u8g2.setCursor(CN_4th_LINE_X,TXT_4th_LINE_Y);
      u8g2.print("短按播放/长按录制"); 

      break;      
    case MODE_PLAYING:
      u8g2.print("播放中");
      cmdDataDis(); 

      // 操作提示
      u8g2.setFont(u8g2_font_wqy12_t_gb2312a);  
      u8g2.setCursor(CN_4th_LINE_X,TXT_4th_LINE_Y);
      u8g2.print("         短按暂停"); 
      break;
    case MODE_RECORD_WAITING:
      u8g2.setFont(u8g2_font_wqy16_t_gb2312);  
      u8g2.setCursor(44,38);
      u8g2.print("录制中"); 

      // 操作提示
      u8g2.setFont(u8g2_font_wqy12_t_gb2312a);  
      u8g2.setCursor(CN_4th_LINE_X,TXT_4th_LINE_Y);
      u8g2.print("     短按退出录制"); 
      break;     
    case MODE_RECORDING:
      if(cmdSize == MAX_CMD_SIZE){
        // 第一行  pausing 1/100
        u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
        u8g2.setCursor(CN_1st_LINE_X,TXT_1st_LINE_Y);
        u8g2.print("已达存储上限");
        cmdDataDis(); 

      }else{
        u8g2.setFont(u8g2_font_wqy16_t_gb2312);  
        u8g2.setCursor(48,38);
        u8g2.print("录制中"); 
      }

      // 操作提示
      u8g2.setFont(u8g2_font_wqy12_t_gb2312a);  
      u8g2.setCursor(CN_4th_LINE_X,TXT_4th_LINE_Y);
      u8g2.print("     短按退出录制"); 
      break;     
  }
 
  u8g2.sendBuffer();
  screenFresh = false;
}

int _getX(int x, int num1, int num2){
  int rs = x;
  if(num1<10){
    rs=rs+u8g2.getMaxCharWidth();
  }
  if(num1<100){
    rs=rs+u8g2.getMaxCharWidth();
  }
  if(num2<10){
    rs=rs+u8g2.getMaxCharWidth();
  }
  if(num2<100){
    rs=rs+u8g2.getMaxCharWidth();
  }
  return rs;
}

// EEPROM 8bit cmdSize, 3200bit cmdARR

// 存储指令
void saveCmd(){

  int totalSize = sizeof(cmd_record) * cmdSize + 1;

  // 初始化EEPROM 
  EEPROM.begin(totalSize); // 100个指令，32字节一个，需要3200个位空间

  // 指令数
  EEPROM.write(0, cmdSize);

  // 获取 指令列表的地址
  uint8_t *p = (uint8_t*)(&CMD_ARR);
  
  // 读取指令到内存
  for(int i=0;i<(totalSize - 1);i++){
    EEPROM.write(i+1, *(p + i));
  }

  EEPROM.commit();
}

// 加载数据
void loadCmd(){
  Serial.println("");
  Serial.print("loadCMD totalSize>>");

  int totalSize = sizeof(CMD_ARR) + 1;

  Serial.println(totalSize);

  // 超过大小，不在读取
  if(totalSize > 4096){
    return;
  }
  
  // 初始化EEPROM 
  EEPROM.begin(totalSize); // 100个指令，32字节一个，需要3200个位空间
  int savedSize = EEPROM.read(0);

  // 判断第一位是否为空
  if(255 == EEPROM.read(0)){
    return;
  }

  // 获取储存的指令数量
  cmdSize = savedSize;

  // 数据校验
  if(cmdSize > MAX_CMD_SIZE){
    return;
  }

  int cmdTotalSize = cmdSize * sizeof(cmd_record);

  // 获取 指令列表的地址
  uint8_t *p = (uint8_t*)(&CMD_ARR);

  // 读取指令到内存
  for(int i=0;i<cmdTotalSize;i++){
    *(p+i)= EEPROM.read(i + 1);
     Serial.println(*(p+i));
  }

  Serial.println("read success");

  EEPROM.commit();
}

void setup() {

  // 开启串口连接
  Serial.begin(115200);
  while(!Serial)
  delay(50);
 
  // 针脚定义
  pinMode(BTN_PIN, INPUT);
  pinMode(RECV_PIN, INPUT);
  pinMode(SEND_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // 模式切换中断
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), keyDown, FALLING);

  // 加载数据
  loadCmd();
  
  // 显示屏初始化
  u8g2.begin();
  u8g2.enableUTF8Print();

  // 红外接收初始化
  irrecv.enableIRIn();

  // 播放初始化
  playerInit();

  // 开启显示
  freshScr();
  Serial.println("");
  Serial.println("started!");
}

// the loop function runs over and over again forever
void loop() {
  keyCommondCallback();// 按键监听
  recordLoop(); //录制监听
  playLoop(); //播放监听
  infoDisplay(); //显示刷新
  if(SYS_MODE == MODE_RECORDING || SYS_MODE == MODE_PLAYING){
    delay(5);
  }else{
    delay(200);
  }  
}

String toUpperCase(String str) {
  for (int i = 0; i < str.length(); i++) {
    if (str[i] >= 'a' && str[i] <= 'z') {
      str[i] = str[i] - 32;
    }
  }
  return str;
}
