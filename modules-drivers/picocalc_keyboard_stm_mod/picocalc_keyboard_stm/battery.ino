#include "battery.h"
#include "reg.h"

static unsigned int low_bat_count =0;

/* Returns true if the driver has taken over LED management */
static bool led_driver_active() {
  return reg_is_bit_set(REG_ID_LED, LED_DRIVER_CTRL);
}

/* Apply REG_ID_LED value to hardware (called on I2C write) */
void led_apply() {
  uint8_t val = reg_get_value(REG_ID_LED);

  /* Green LED (PC13, active-low) */
  digitalWrite(PC13, (val & LED_GREEN_MASK) ? LOW : HIGH);

  /* Orange LED (PMU charge LED) */
  uint8_t orange = val & LED_ORANGE_MASK;
  if (orange == LED_ORANGE_OFF) {
    PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);
  } else if (orange == LED_ORANGE_ON) {
    PMU.setChargingLedMode(XPOWERS_CHG_LED_ON);
  } else if (orange == LED_ORANGE_BLINK1) {
    PMU.setChargingLedMode(XPOWERS_CHG_LED_BLINK_1HZ);
  } else if (orange == LED_ORANGE_BLINK4) {
    PMU.setChargingLedMode(XPOWERS_CHG_LED_BLINK_4HZ);
  } else if (orange == LED_ORANGE_AUTO) {
    PMU.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
  }
}

void indicator_led_on(){
    digitalWrite(PC13, LOW);
}

void indicator_led_off(){
    digitalWrite(PC13, HIGH);
}

void flash_one_time(int ts,int restore_status) {
    for(int i=0;i<ts;i++) {
      indicator_led_on();
      delay(400);
      indicator_led_off();
      delay(200);
    }
    digitalWrite(PC13,restore_status);
}

void show_bat_segs(){
  if(!PMU.isBatteryConnect()) return;

  int pcnt =  PMU.getBatteryPercent();
  int last_d201_status = digitalRead(PC13);
  
  if(pcnt >0 && pcnt < 33) {
    //show one time
    flash_one_time(1,last_d201_status);
  }else if(pcnt >= 33 && pcnt <66){
    //show 2 times
     flash_one_time(2,last_d201_status);   
  }else if(pcnt >=66 && pcnt <= 100){
    //show 3 times
     flash_one_time(3,last_d201_status);
  }
  
  if(PMU.isCharging()){
    start_chg();
  }

}

void low_bat(){
  if (led_driver_active()) return;  /* driver controls LEDs */
  int pcnt = PMU.getBatteryPercent();
  if(pcnt >=0 && pcnt <= LOW_BAT_VAL){
    low_bat_count++;
      //This is related to the battery charging and discharging logic. If you're not sure what you're doing, please don't modify it, as it could damage the battery.
    indicator_led_off();      
    if(pcnt <= 1) {//This is related to the battery charging and discharging logic. If you're not sure what you're doing, please don't modify it, as it could damage the battery.
      PMU.setChargingLedMode(XPOWERS_CHG_LED_BLINK_4HZ);
      if(pcnt==0 && low_bat_count >= 4 ) {//This is related to the battery charging and discharging logic. If you're not sure what you're doing, please don't modify it, as it could damage the battery.
        PMU.shutdown();//This is related to the battery charging and discharging logic. If you're not sure what you're doing, please don't modify it, as it could damage the battery.
      }
    }else{
      PMU.setChargingLedMode(XPOWERS_CHG_LED_ON);
    }
  }else{
    low_bat_count = 0;
    indicator_led_on();
    PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);
  }
}

void start_chg(){
  if (led_driver_active()) return;  /* driver controls LEDs */
  indicator_led_on();
  PMU.setChargingLedMode(XPOWERS_CHG_LED_BLINK_1HZ);
}

void stop_chg(){
  if (led_driver_active()) return;  /* driver controls LEDs */
  PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);
  low_bat();
}
