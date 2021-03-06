/******************************************************************************************************************//**
 * @brief  Arduino Sketch firmware to be uploaded onto the AgIrrigationRemoteControl Hand-Remote Device.
 * @see https://github.com/tgit23/AgIrrigationRemoteControl
 * @remarks Version 2017.06.17
 * @todo
 *  - Implement Firmata for Base/Desktop operation
 * @authors 
 *    tgit23        01/2017       Original
 *    tgit23        07/2017       Implemented keypad button interrupts and Non-Blocking functionality
**********************************************************************************************************************/
#include <LiquidCrystal.h>
#include <EEPROM.h>
#include <PeerIOSerialControl.h>        //See https://github.com/tgit23/PeerIOSerialControl
#include <SSoftwareSerial.h>            //See 

//=====================================================================================================================
//------------------------------ SIMPLE USER CONFIGURATION SETTINGS ---------------------------------------------------
//=====================================================================================================================
#define BUILD_VERSION                     20170706  // Release Version used to Build the Unit ( without the dots )
#define TRANSCEIVER_ID                    1         // Unique numeric (ID)entity for this Unit(1-15)
#define XBEECONFIG                        0         // Configure the XBEE using XCTU Digi Software by setting this to 1
#define ULTRASONIC_WATER_LEVEL_INSTALLED  0         // 0 = NO, 1 = YES ( Wire TRIG -> D4, ECHO -> D5 )
#define WATER_PRESSURE_INSTALLED          0         // 0 = NO, 1 = YES ( Wire SENSE -> A3 )

//^^^[ END - SIMPLE USER CONFIGURATION SETTINGS ]^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//---------------------------------------------------------------------------------------------------------------------

//---[ PROGRAM BEHAVIOR ]----------------------------------------------------------------------------------------------
#define WAIT_REPLY_MS         3000      // How long to wait for an XBee reply
#define START_STATUS_ITERATE  30000     // Start iterating Menu-Items after idle for (ms)
#define ITERATE_EVERY         5000      // Iterate Menu-Items every (ms); when idle
#if XBEECONFIG>0
#define NONBLOCKING           0         // Digi-Xbee-XCTU configuration software has problems with active interupts
#else
#define NONBLOCKING           1         // Blocking mode (0) stalls screen till item is gotten, (1) releases screen
#endif
#define DEBUG                 0         // Set this to 1 for Serial DEBUGGING messages ( Firmware development use only )
#if DEBUG>0                             // Activate Debug Messages
  #define DBL(x) Serial.println x
  #define DB(x) Serial.print x
  #define DBC Serial.print(", ")
#else                                   // ELSE - Clear Debug Messages
  #define DBL(x)
  #define DB(x)
  #define DBC  
#endif

//---[ HAND-REMOTE PIN SETTINGS ]--------------------------------------------------------------------------------------
LiquidCrystal lcd(8, 9, 4, 5, 6, 7);    // Pins used by the LCD Keypad Shield
#if BUILD_VERSION>20170524
  #define SS_TX_PIN 2                     // TX -> XBEE-DIN ( Closest to UNO )
  #define SS_RX_PIN 3                     // RX -> XBEE-DOUT ( Farthest from UNO )
  #define SBUZZ 12                        // Buzzer Signal Pin (S)
  #define PBUZZ 13                        // Buzzer Power Pin (+)
#else                                   // vvvvvvvv [ Build Release 2017.05.24 Pins ] vvvvvvvvvvvvvv
  #define SS_TX_PIN 11                    // TX -> XBEE-DIN ( Closest to UNO )
  #define SS_RX_PIN 12                    // RX -> XBEE-DOUT ( Farthest from UNO )
  #define SBUZZ 2                         // Buzzer Signal Pin (S)
#endif                                  // ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
#define BATT_R1_VINTOA1   1500          // Integer Value of BATTVOLT Resistor VIN -> A1 in KOhms
#define BATT_R2_A1TOGND   510           // Integer Value of BATTVOLT Resistor A1 -> GND in KOhms

//---[ PROGRAM CONSTANTS ]---------------------------------------------------------------------------------------------
enum Button       { UP, DOWN, RIGHT, LEFT, SELECT, NONE };
enum eValModifier { RAW, PRESSURE, BATTVOLTS };
#define NOPIN     0xFF                // 255 (unit8_t) = 'NOPIN'
#define MAIN      0                   // Menu[#].Sub[0-MAIN] for currently-read value of the Menu Item
#define VALID     true                // Is Menu[#].Sub[MAIN].State a 'VALID' (read) value?
#define SET       1                   // Menu[#].Sub[1-SET] for SET values to await being applied to the MAIN-Value
#define SETTABLE  true                // Does Menu[#].Sub[SET].State allow 'SETTABLE' values by the user
#define LOALARM   2                   // Menu[#].Sub[2-LOALARM] for storing Low Alarm value to test the MAIN-Value
#define HIALARM   3                   // Menu[#].Sub[3-HIALARM] for storing High Alarm value to test the MAIN-Value
#define ON        true                // Is the "Menu[#].Sub[??ALARM].State" (ALARM) ON or NOT-ON?

//---[ Globals ]-------------------------------------------------------------------------------------------------------
SSoftwareSerial IOSerial(SS_RX_PIN,SS_TX_PIN);              // SoftSerial for XBEE ( rxPin, txPin ) - allows interrupts
PeerIOSerialControl XBee(TRANSCEIVER_ID,IOSerial,Serial);   // XBee(ArduinoID, IOSerial, DebugSerial)
void GetItem(int i = -1);                                   // Predefine headers so SetupMenu() can be at top
void CheckAlarms(int i = -1);
void EEPROMGet(int i = -1);

volatile unsigned long last_bpress_millis = 0;              // Track last button press time
Button last_bpress = NONE;                                  // Store last button press for processing
Button prev_bpress = NONE;                                  // Used to count ButtonHeld counter
unsigned long wait_reply = 0;                               // Track non-blocking reply time
unsigned long last_iter_millis = 0;                         // Track last status iteration time

int idx = 0;                                                // Track Menu index item
int SubIdx = 0;                                             // Track current menu value item
int ButtonHeld = 0;                                         // Increment values by 10 when button is held
bool AlarmActive = false;                                   // Track if an Active Alarm is present
bool bIterating = false;                                    // Alarm only active while iterating the menu
int MenuItemsIdx = 0;                                       // Track the Menu Index (mi)
int PacketID = -1;                                          // For non-blocking communications

//---[ Menu-Item Structure and Constants ]-----------------------------------------------------------------------------
#define BATT 0                            // Menu[BATT] monitors the battery voltage level
#define MAX_MENU_ITEMS 15                 // Maximum number of Menu Items allowed ( using 71% dynamic memory )
#define MAXOPTIONS 2                      // Maximum number of Menu Item Options allowed
struct uDevices {
  const char      *Text;                  // Text to designate the Unit a Menu Item will control
  int             TransceiverID = 0;      // The TransceiverID set on the Unit the Menu Item will control
};
struct uMenuOption {
  const char      *Text;                  // Text display for this-one Menu-Item OPTION
  int             Value = LOW;            // A value used for this OPTION ( HIGH-1/LOW-0 for ON/OFF / TransieverID)
};
struct uSubVal {
  int             Value = -1;             // Value Pin for Val[MAIN, SET, HIALARM, LOALARM]
  int             ToneHz = 1000;          // A tone frequency associated with this alarm when activated
  char            ID = 0;                 // Character to IDentify an alarm; setting an ID makes an alarm SETTABLE
  bool            State = false;          // [MAIN]State=VALID, [SET]State=SETTABLE, [xxALARM]=ON
};
struct MenuItems {
  uDevices        Device;                 // The device the Menu Item is for
  const char      *Text;                  // The text to display on the LCD for this Menu item
  uint8_t         Pin = NOPIN;            // Where to get/set the MAIN-Value; LOCAL, REMOTE, NOPIN, etc...
  uSubVal         Sub[4];                 // MAIN, SET, LOALARM, HIALARM - Value storage per Menu Item
  uMenuOption     Option[MAXOPTIONS];     // Selectable options like ON/OFF, Pump1 or 2 etc..
  uint8_t         LastOptionIdx = 0;      // The number of "Option"s for this Menu-Item; or 0 = Numeric value
  eValModifier    ValueModifier = RAW;    // Select value modifying equations in LCD_display()
} Menu[MAX_MENU_ITEMS];                   

//=====================================================================================================================
//------------------------------ MENU STRUCTURE ( ADVANCED CONFIGURATION ) --------------------------------------------
//=====================================================================================================================
/******************************************************************************************************************//**
 * @brief  Setup the LCD menu
 * @remarks
 * - Allows a single spot customization to the user interface
 * - Display will show the items in the same order as they are defined here
 * @code
 *   exmaple code
 * @endcode
**********************************************************************************************************************/
uDevices HandRemote, CanalPump, DitchPump;                  // Name and Define all controllable devices in the System
void SetupMenu() {

  HandRemote.Text = "Hand Remote"; HandRemote.TransceiverID = TRANSCEIVER_ID;
  DitchPump.Text = "Ditch Pump"; DitchPump.TransceiverID = 11;
  CanalPump.Text = "Canal Pump"; CanalPump.TransceiverID = 10;
  
  //BATT (idx-0) ----------------------------
  Menu[BATT].Device = HandRemote;
  Menu[BATT].Pin = A1;                                      // Battery level is gotten from the Hand-Remote pin A1
  Menu[BATT].Text = "Battery(B)";                           // Create a menu item for monitoring the Battery
  Menu[BATT].ValueModifier = BATTVOLTS;                     // Modify raw value to show voltage
  Menu[BATT].Sub[LOALARM].ID = 'b';                         // A Low Alarm is identified by a lower-case 'b'
   
  //-----------------------------------------
  MenuItemsIdx++;
  idx = MenuItemsIdx;                                       // !!!-- Set where the Menu will start --!!!
  Menu[MenuItemsIdx].Device = DitchPump;
  Menu[MenuItemsIdx].Pin = 7;                               // Power is set/got on all Pump-Controller's on pin [D7]
  Menu[MenuItemsIdx].Text = "Power(P)";                     // Create a menu item for Power Control
  Menu[MenuItemsIdx].Sub[SET].State = SETTABLE;             // Allow this Value to be 'SET' by the user
  Menu[MenuItemsIdx].Sub[LOALARM].ID = 'p';                 // A Low Alarm is identified by a lower-case 'p'
  Menu[MenuItemsIdx].Sub[HIALARM].ID = 'P';                 // A High Alarm is identified by an upper-case 'P'
  Menu[MenuItemsIdx].Option[0].Text = "Off";                // Power can be "Off"             - Option #0 = Off
  Menu[MenuItemsIdx].Option[0].Value = LOW;                 // "Off" will be the value 'LOW"  - Off = LOW
  Menu[MenuItemsIdx].Option[1].Text = "On";                 // Power can be "On"              - Option #1 = On
  Menu[MenuItemsIdx].Option[1].Value = HIGH;                // "On" will be the value 'HIGH'  - On = HIGH
  Menu[MenuItemsIdx].LastOptionIdx = 1;                     // Last Option Index defined      - Number of Options - 1  
  
  //-----------------------------------------
#if ULTRASONIC_WATER_LEVEL_INSTALLED>0
  MenuItemsIdx++;
  Menu[MenuItemsIdx].Device = DitchPump;
  Menu[MenuItemsIdx].Pin = 64;                              // Water Level is read from VIRTUAL (Pump-Controllers firmware) pin 64           
  Menu[MenuItemsIdx].Text = "Water (L)";                    // Create a menu item for Water Level Transducer
  Menu[MenuItemsIdx].Sub[LOALARM].ID = 'l';                 // A Low Alarm is identified by a lower-case 'l'
  Menu[MenuItemsIdx].Sub[HIALARM].ID = 'L';                 // A High Alarm is identified by an upper-cse 'L'
#endif
  //-----------------------------------------
#if WATER_PRESSURE_INSTALLED>0
  MenuItemsIdx++;
  Menu[MenuItemsIdx].Device = DitchPump;
  Menu[MenuItemsIdx].Pin = A3;                              // The 'signal' is gotten on all Pump-Controllers on pin [A3]
  Menu[MenuItemsIdx].Text = "Pressure(R)";                  // Create a menu item for the Primary Pressure Transducer
  Menu[MenuItemsIdx].ValueModifier = PRESSURE;              // Modify value to display PSI instead of MPa
  Menu[MenuItemsIdx].Sub[LOALARM].ID='r';                   // A Low Pressure alarm is identified by a lower-case 'r'
  Menu[MenuItemsIdx].Sub[HIALARM].ID='R';                   // A High Pressure alarm is identified by an upper-case 'R'
#endif
/*// UN-COMMENT BELOW FOR A SECOND PUMP-CONTROLLER ( NAMED "Canal Pump" ) WITH TRANSCEIVER_ID = 11  
  //-----------------------------------------
  MenuItemsIdx++;
  Menu[MenuItemsIdx].Text = "Power(C)";                     // Create a menu item for Power Control
  Menu[MenuItemsIdx].Device = CanalPump;
  Menu[MenuItemsIdx].Pin = 7;                               // Power is set/got on all Pump-Controller's on pin [D7]
  Menu[MenuItemsIdx].Sub[SET].State = SETTABLE;             // Allow this Value to be 'SET' by the user
  Menu[MenuItemsIdx].Sub[LOALARM].ID = 'c';                 // A Low Alarm is identified by a lower-case 'p'
  Menu[MenuItemsIdx].Sub[HIALARM].ID = 'C';                 // A High Alarm is identified by an upper-case 'P'
  Menu[MenuItemsIdx].Option[0].Text = "Off";                // Power can be "Off"             - Option #0 = Off
  Menu[MenuItemsIdx].Option[0].Value = LOW;                 // "Off" will be the value 'LOW"  - Off = LOW
  Menu[MenuItemsIdx].Option[1].Text = "On";                 // Power can be "On"              - Option #1 = On
  Menu[MenuItemsIdx].Option[1].Value = HIGH;                // "On" will be the value 'HIGH'  - On = HIGH
  Menu[MenuItemsIdx].LastOptionIdx = 1;                     // Last Option Index defined      - Number of Options - 1  

  //-----------------------------------------
  MenuItemsIdx++;
  Menu[MenuItemsIdx].Text = "Pressure(F)";                  // Create a menu item for the Primary Pressure Transducer
  Menu[MenuItemsIdx].Device = CanalPump;
  Menu[MenuItemsIdx].Pin = A3;                              // The 'signal' is gotten on all Pump-Controllers on pin [A3]
  Menu[MenuItemsIdx].ValueModifier = PRESSURE;              // Modify value to display PSI instead of MPa
  Menu[MenuItemsIdx].Sub[LOALARM].ID='f';                   // A Low Pressure alarm is identified by a lower-case 'r'
  Menu[MenuItemsIdx].Sub[HIALARM].ID='F';                   // A High Pressure alarm is identified by an upper-case 'R'

  //-----------------------------------------
  MenuItemsIdx++;
  Menu[MenuItemsIdx].Text = "Pressure(S)";                  // Create a menu item for the Secondary Pressure Transducer
  Menu[MenuItemsIdx].Device = CanalPump;
  Menu[MenuItemsIdx].Pin = A4;                              // Menu-item is for Pump-Option #0 (Canal) on Pin (A4)
  Menu[MenuItemsIdx].ValueModifier = PRESSURE;              // Modify value to display PSI instead of MPa
  Menu[MenuItemsIdx].Sub[LOALARM].ID='s';                   // A Low Pressure alarm is identified by a lower-case 's'
  Menu[MenuItemsIdx].Sub[HIALARM].ID='S';                   // A High Pressure alarm is identified by an upper-case 'S'
  */
  //------------[ Start-Up the Display ( DO NOT CHANGE! )]-------------  
  for ( int i = 0; i <= MenuItemsIdx; i++ ) {
    if ( Menu[i].Sub[LOALARM].ID != 0 || Menu[i].Sub[HIALARM].ID != 0 ) {
      EEPROMGet(i);                                         // Load Alarm values from EEPROM
    }
  }
  GetItem();                                                // Get starting Menu item
  LCD_display();                                            // Update the display
}

/******************************************************************************************************************//**
 * @brief  Function to modify raw values into meaningful information
 * @code
 *   exmaple code
 * @endcode
**********************************************************************************************************************/
int ValueModify(int Index, int SubIndex, int AddBy = 0) {
  int RetVal = 0;       // Return value
  int iVal = 0;         // Initial value before adding or subtracting
  bool bOnce = true;
  
  do {  //------------ Modify RAW values to meaningful Values ---------------------------------------------
    
    // Change Pressure MPa Raw value to PSI ( pounds per square inch )
    if ( Menu[Index].ValueModifier == PRESSURE ) {
      RetVal = (int) ((Menu[Index].Sub[SubIndex].Value - 97) * 0.2137);

    // Calculate voltage from Resistors used in Voltage Divider
    } else if ( Menu[Index].ValueModifier == BATTVOLTS ) {
      RetVal = (long(Menu[Index].Sub[SubIndex].Value)*BATT_R1_VINTOA1)/long(1.6*BATT_R2_A1TOGND);

    // No Modification - just return raw value
    } else {
      RetVal = Menu[Index].Sub[SubIndex].Value;
    }

    // -------------- Change meaningful Value by 'AddBy' --------------------------------------------------
    if ( AddBy == 0 ) break;
    if (bOnce) {
      iVal = RetVal; bOnce = false; continue;
    } else {
      if( AddBy>0 ) { Menu[Index].Sub[SubIndex].Value++; } else { Menu[Index].Sub[SubIndex].Value--; }
    }

    // If adding - keep adding till the return value is no longer less than 'AddBy'
    // If subtracting - keep subtracting till the return value is no longer greater than 'AddBy'
  } while ( (AddBy > 0 && (RetVal < iVal + AddBy)) || ( AddBy < 0 && (RetVal > iVal + AddBy)) );
  

  return RetVal;
}

/******************************************************************************************************************//**
 * @brief  Record Menu Item Values in EEPROM non-volitale memory.
 * @remarks
 * - Arduino UNO offers 1024-bytes or (146)7-Byte Menu Item Storage
 * - Pump-Controller Menu Items need 5-Alarm Bytes per Selectable Pump
 * - Hand_NOPIN Menu Items need 7-Bytes ( 2-Value, 5-Alarms )
 *  @code
 *    exmaple code
 *  @endcode
**********************************************************************************************************************/
void EEPROMSet(int i = -1) {
  if ( i == -1 ) i = idx;DB(("EEPROMSet("));DB((i));DBL((")"));
  int iOffset = (i*5);      // (5)Bytes per Menu Item
    
  // Store Alarm Values in EEPROM
  byte loAlarmLoByte = ((Menu[i].Sub[LOALARM].Value >> 0) & 0xFF);
  byte loAlarmHiByte = ((Menu[i].Sub[LOALARM].Value >> 8) & 0xFF);
  byte hiAlarmLoByte = ((Menu[i].Sub[HIALARM].Value >> 0) & 0xFF);
  byte hiAlarmHiByte = ((Menu[i].Sub[HIALARM].Value >> 8) & 0xFF);
  byte AlarmSet = 0x22;   // 2=OFF, A=ON
  if ( Menu[i].Sub[LOALARM].State == ON ) bitSet(AlarmSet,3);
  if ( Menu[i].Sub[HIALARM].State == ON ) bitSet(AlarmSet,7);
    
  EEPROM.update( iOffset, loAlarmLoByte);iOffset++;
  DB(("EEPROM.update( "));DB((iOffset));DBC;DB((loAlarmLoByte, HEX));DBL((")"));
  EEPROM.update( iOffset, loAlarmHiByte);iOffset++;
  DB(("EEPROM.update( "));DB((iOffset));DBC;DB((loAlarmHiByte, HEX));DBL((")"));
  EEPROM.update( iOffset, hiAlarmLoByte);iOffset++;
  DB(("EEPROM.update( "));DB((iOffset));DBC;DB((hiAlarmLoByte, HEX));DBL((")"));
  EEPROM.update( iOffset, hiAlarmHiByte);iOffset++;
  DB(("EEPROM.update( "));DB((iOffset));DBC;DB((hiAlarmHiByte, HEX));DBL((")"));
  EEPROM.update( iOffset, AlarmSet);iOffset++;
  DB(("EEPROM.update( "));DB((iOffset));DBC;DB((AlarmSet, HEX));DBL((")"));
}

/******************************************************************************************************************//**
 * @brief  Read Menu Item Values from Arduino EEPROM non-volitale memory.
 * @see    EEPROMSet for Addressing notation
 * @code
 *   exmaple code
 * @endcode
**********************************************************************************************************************/
void EEPROMGet(int i) {
  byte StatusByte = 0;
  if ( i == -1 ) i = idx;DB(("EEPROMGet("));DB((i));DB((")"));
  int iOffset = (i*5);                                          // (5)Bytes per Menu Item

  // Get Alarm Values from EEPROM
  DB(("EEPROMGet() - LoAlarmByte Offset = "));DB((iOffset));DBL((")"));
  byte loAlarmLoByte = EEPROM.read( iOffset );iOffset++;
  byte loAlarmHiByte = EEPROM.read( iOffset );iOffset++;
  byte hiAlarmLoByte = EEPROM.read( iOffset );iOffset++;
  byte hiAlarmHiByte = EEPROM.read( iOffset );iOffset++;
  byte AlarmSet = EEPROM.read( iOffset );iOffset++;
  
  Menu[i].Sub[LOALARM].Value = (int)((loAlarmLoByte << 0) & 0xFF) + ((loAlarmHiByte << 8) & 0xFF00);
  Menu[i].Sub[HIALARM].Value = (int)((hiAlarmLoByte << 0) & 0xFF) + ((hiAlarmHiByte << 8) & 0xFF00);
  Menu[i].Sub[LOALARM].State = bitRead(AlarmSet,3);
  Menu[i].Sub[HIALARM].State = bitRead(AlarmSet,7);

  // NOPIN Values are 'VALID' if the AlarmSet Bit Check passes ( Value was set/not just garbage )
  if ( Menu[i].Pin == NOPIN ) {
    if ( (AlarmSet & 0x77) == 0x22 ) Menu[i].Sub[MAIN].State = VALID;
  }
}

/******************************************************************************************************************//**
 * @brief  Obtain menu values
 * @remarks
 *  - Battery read 458 when running off USB and 856 when running from 9VDC Battery.
 * @code
 *   exmaple code
 * @endcode
**********************************************************************************************************************/
void GetItem(int i) {
  
  if ( i == -1 ) i = idx;                                                         // Default index = global 'idx'
  DB(("GetItem("));DB((i));DBL((")"));                
  if ( Menu[i].Pin == NOPIN ) return;

  Menu[i].Sub[MAIN].State = !VALID;                                               // Invalidate last reading
  if ( Menu[i].Device.TransceiverID == TRANSCEIVER_ID ) {                         // Local Pin Read
    if ( Menu[i].Pin >= A0 ) {                                                    // Read value from Hand-Remote Pin
      Menu[i].Sub[MAIN].Value = analogRead(Menu[i].Pin);
    } else {
      Menu[i].Sub[MAIN].Value = digitalRead(Menu[i].Pin);
    }
    Menu[i].Sub[MAIN].State = VALID;            // Validate local Pin Values
    CheckAlarms(i);                             // Check the value for Alarms
  } else {                                                                        // Remote Pin Read; Set TransceiverID
    if ( Menu[i].Device.TransceiverID != XBee.TargetArduinoID() ) XBee.TargetArduinoID( Menu[i].Device.TransceiverID );
#if NONBLOCKING>0
    if ( Menu[i].Pin >= A0 ) {
        wait_reply = millis();
        PacketID = XBee.analogReadNB(Menu[i].Pin);
        DB(("PacketID="));DBL((PacketID));
    } else {
        wait_reply = millis();
        PacketID = XBee.digitalReadNB(Menu[i].Pin);
        DB(("PacketID="));DBL((PacketID));
    }
#else
    if ( Menu[i].Pin >= A0 ) {
      Menu[i].Sub[MAIN].Value = XBee.analogReadB(Menu[i].Pin);
    } else {
      Menu[i].Sub[MAIN].Value = XBee.digitalReadB(Menu[i].Pin);
    }
    if ( Menu[i].Sub[MAIN].Value != -1 ) {
      Menu[i].Sub[MAIN].State = VALID;            // Validate Pin Values that return a legitimate value
      CheckAlarms(i);                             // Check the value for Alarms
    }
#endif
  }
}

/******************************************************************************************************************//**
 * @brief  Checks a Menu items 'Value' with its LOALARM and HIALARM values and trigger the buzzer if needed.
 * @remarks
 * - Only the NOT-EQUAL alarm will trigger during a communications error; all others will be disabled.
 * - Alarms are only checked while the unit is in idle iteration mode.
 * - Battery levels under 550 are disabled to allow USB Power without triggering the alarm.
 * @code
 *   exmaple code
 * @endcode
**********************************************************************************************************************/
void CheckAlarms(int i) {
  if ( i == -1 ) i = idx;                                                         // Default index = global 'idx'
  DB(("AlarmCheck("));DB((i));DBL((")"));
  
  if ( !bIterating ) return;                                                      // No Alarms unless iterating
  bool bOption = ( Menu[i].LastOptionIdx > 0 );
  bool bLoAlarmOn = ( Menu[i].Sub[LOALARM].State == ON && Menu[i].Sub[LOALARM].ID != 0 );
  bool bHiAlarmOn = ( Menu[i].Sub[HIALARM].State == ON && Menu[i].Sub[HIALARM].ID != 0 );

  if ( bOption && bHiAlarmOn ) AlarmActive = ( Menu[i].Sub[MAIN].Value != Menu[i].Sub[HIALARM].Value ); // NOT-EQUAL
  
  if ( Menu[i].Sub[MAIN].State == VALID && !AlarmActive ) {                       // All Other Alarms are OFF if ERR
    if ( !bOption ) {                                                             // Compare numeric Values
      if ( bHiAlarmOn ) AlarmActive = ( Menu[i].Sub[MAIN].Value > Menu[i].Sub[HIALARM].Value );         // GREATER-THAN
      if ( bLoAlarmOn && !AlarmActive ) AlarmActive = ( Menu[i].Sub[MAIN].Value < Menu[i].Sub[LOALARM].Value ); // LESS-THAN
      if ( i == BATT && Menu[i].Sub[MAIN].Value < 550 ) AlarmActive = false;      // Disable Low BATT for USB Plug-In
    } else {
      if ( bLoAlarmOn) AlarmActive = ( Menu[i].Sub[MAIN].Value == Menu[i].Sub[LOALARM].Value ); // Option Compare EQUALS
    }
  }
  if ( AlarmActive ) tone(SBUZZ,Menu[i].Sub[LOALARM].ToneHz);                     // Sound Buzzer

}

/******************************************************************************************************************//**
 * @brief  Preform value setting and recording when the 'Select' button is pressed.
 * @remarks
 * - Setting values implements blocking; without it the read-back 'GetItem()' doesn't function correctly.
 * @code
 *   exmaple code
 * @endcode
**********************************************************************************************************************/
void SetItem(int i = -1) {
  if ( i == -1 ) i = idx;int iSetValue = 0;DBL(("SetItem()"));

  if ( SubIdx == LOALARM || SubIdx == HIALARM ) {                         // ----------- ALARMS -----------------
    if (Menu[i].Sub[SubIdx].State == ON) {Menu[i].Sub[SubIdx].State = !ON;} else {Menu[i].Sub[SubIdx].State = ON;}
    EEPROMSet();                                                          // Save Alarm ON/OFF Status in EEPROM

  } else if ( SubIdx == MAIN ) {                                          // ----------- MAIN -------------------
    GetItem(i);                                                           // Select will Refresh item

  } else if ( SubIdx == SET ) {                                           // ----------- SET --------------------
    iSetValue = Menu[i].Sub[SET].Value;                                   // Record the current Set Value
    if ( Menu[i].LastOptionIdx > 0 ) {                                                // IF [SET] value is an OPTION
      if (Menu[i].Sub[SET].Value < 0 || Menu[i].Sub[SET].Value > MAXOPTIONS) return;  // Boundary Check the OPTION
      iSetValue = Menu[i].Option[Menu[i].Sub[SET].Value].Value;                       // Record the OPTIONS [SET] Value
    }
    
    if ( Menu[i].Pin != NOPIN ) {                                         // ----- HARDWARE SET ------------------
      Menu[i].Sub[MAIN].State = !VALID;                                   // UN-VALIDATE the [MAIN] Value; its changing
      if ( Menu[i].Device.TransceiverID == TRANSCEIVER_ID ) {
        if ( Menu[i].Pin >= A0 ) {
          analogWrite(Menu[i].Pin, iSetValue);
        } else {
          digitalWrite(Menu[i].Pin, iSetValue);
        }
      } else {
        if ( Menu[i].Device.TransceiverID != XBee.TargetArduinoID() ) XBee.TargetArduinoID( Menu[i].Device.TransceiverID );
        if ( Menu[i].Pin >= A0 ) {
          XBee.analogWriteB(Menu[i].Pin, iSetValue);                  // Set the Remote Arduino Analog Pin
        } else {
          XBee.digitalWriteB(Menu[i].Pin, iSetValue);                 // Set the Remote Arduino Digital Pin
        }
      }
    }
    
  }
}

/******************************************************************************************************************//**
 * @brief  Properly display a Menu Item on the LCD screen
 * @code
 *   exmaple code
 * @endcode
**********************************************************************************************************************/
void LCD_display() {

//--- FORWARD SERIAL TO XBEE for XBEECONFIG --------------------
#if XBEECONFIG>0
  // Top Row (Selected Pump)
  lcd.clear();lcd.setCursor(0,0);
  lcd.print( "XBEE Config Mode" );
#else
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

  // Top Row (Selected Pump)
  lcd.clear();lcd.setCursor(0,0);
  lcd.print( Menu[idx].Device.Text );
  
  // Right Side Alarm Identifiers
  int pos = 15;
  for (int i=0;i<=MenuItemsIdx;i++) {
    if ( Menu[i].Sub[LOALARM].State == ON && Menu[i].Sub[LOALARM].ID != 0 ) {
        lcd.setCursor(pos,0);lcd.print(Menu[i].Sub[LOALARM].ID);pos--;}
    if ( Menu[i].Sub[HIALARM].State == ON && Menu[i].Sub[HIALARM].ID != 0 ) {
        lcd.setCursor(pos,0);lcd.print(Menu[i].Sub[HIALARM].ID);pos--;}
  }
       
  // Bottom Row ( Menu Item Text ) and Function
  lcd.setCursor(0,1);lcd.print(Menu[idx].Text);
  lcd.setCursor(strlen(Menu[idx].Text), 1);
  
  if ( SubIdx == MAIN ) {
    lcd.print(" =");
    lcd.setCursor( strlen(Menu[idx].Text) + 2, 1);
  } else if ( SubIdx == SET ) {
    lcd.print(" SET");
    lcd.print((char)126); //Character '->'
    lcd.setCursor( strlen(Menu[idx].Text) + 5, 1);
  } else {
    if ( Menu[idx].LastOptionIdx > 0 ) {
      if ( SubIdx == LOALARM ) lcd.print(" =!");
      if ( SubIdx == HIALARM ) {lcd.print(" ");lcd.print((char)183);lcd.print("!");} // Slashed equal
    } else {
      if ( SubIdx == LOALARM) lcd.print(" <!");
      if ( SubIdx == HIALARM) lcd.print(" >!");
    }
    lcd.setCursor( strlen(Menu[idx].Text) + 3, 1);
  }
  
  // Bottom Row ( Display Value )
  if ( SubIdx == MAIN && Menu[idx].Sub[MAIN].State != VALID ) {
#if NONBLOCKING>0
    if ( PacketID != -1 ) { lcd.print("?"); } else { lcd.print("ERR"); }
#else
    lcd.print("ERR");
#endif
  } else {
    if ( Menu[idx].LastOptionIdx > 0 ) {
      if ( Menu[idx].Sub[SubIdx].Value < 0 || Menu[idx].Sub[SubIdx].Value > Menu[idx].LastOptionIdx ) Menu[idx].Sub[SubIdx].Value = 0;
      lcd.print(Menu[idx].Option[Menu[idx].Sub[SubIdx].Value].Text); 
    } else {
      lcd.print( ValueModify(idx,SubIdx) );
    }
  }
#endif
}

/******************************************************************************************************************//**
 * @brief  Checks and Debounces button presses on the LCD Keypad
 * @remarks
 * - This function is called automatically over-and-over by Arduino
 * - Line 'if ( clk - last_bpress_millis < 500 )' only allows a new button press to register every 500ms.
 * @code
 *   ButtonCheck(analogRead(0));
 * @endcode
**********************************************************************************************************************/
void ButtonCheck(int adc_value) {
  unsigned long clk = millis();
  if ( clk - last_bpress_millis < 350 ) return;         // Debounce button presses

  if (adc_value > 1000) { last_bpress = NONE; }
  else if (adc_value < 50) { last_bpress = RIGHT; }
  else if (adc_value < 195) { last_bpress = UP; }
  else if (adc_value < 380) { last_bpress = DOWN; }
  else if (adc_value < 555) { last_bpress = LEFT; }
  else if (adc_value < 790) { last_bpress = SELECT; }

  if ( prev_bpress == last_bpress ) { ButtonHeld++; } else { ButtonHeld=0; }
  prev_bpress = last_bpress;
  if ( last_bpress != NONE ) last_bpress_millis = millis();
}

#if NONBLOCKING>0
/******************************************************************************************************************//**
 * @brief  ISR ( Interrupt Service Routine ) for Keypad Up, Down, and Right arrow buttons.
 * @remarks
 * - PCINT1_vect Pin Change Interrupt will not trigger on Left or Select buttons ( digital threshold? )
 * - The interrupt stores the button pressed by calling ButtonCheck() and processes it when the loop() is called.
 * - The original SoftwareSerial Library calls ALL Interrupts so a modified 'SSoftwareSerial' must be used to compile
**********************************************************************************************************************/
ISR(PCINT1_vect) {
  ButtonCheck(analogRead(0));
}
#endif
/******************************************************************************************************************//**
 * @brief  Arduino Sketch Setup routine - Initialize the environment.
 * @remarks
 * - Setup() is called once; automatically when Arduino UNO is first powered on or reset.
 * - pin#10 INPUT Backlit shorting see http://forum.arduino.cc/index.php?topic=96747.0
**********************************************************************************************************************/
void setup(){

  pinMode(10, INPUT);           // Fix for Q1-LCD Backlit shorting issue
  pinMode(A1, INPUT);           // A0 Controlled by LCD-Display library.
  pinMode(A2, INPUT_PULLUP);    // A1 is used by the Battery Level Indicator.
  pinMode(A3, INPUT_PULLUP);    // Keep all other Analog pins from floating
  pinMode(A4, INPUT_PULLUP);    // so 'PCINT1_vect' interrupt only triggers
  pinMode(A5, INPUT_PULLUP);    // when analog pin A0 changes
#if NONBLOCKING>0
  noInterrupts();               // switch interrupts off while messing with their settings  
  PCICR =0x02;                  // Enable 'PCIE1' bit of PCICR Pin Change Interrupt the PCINT1 interrupt
  PCMSK1 = 0b00000001;          // Pin Change Interrupt Mask ( NA, RESET, A5, A4, A3, A2, A1, A0 ) - Activate A0              
  interrupts();                 // turn interrupts back on
#endif
  
#ifdef PBUZZ
  pinMode(PBUZZ,OUTPUT);digitalWrite(PBUZZ, HIGH);  // Supply Power to Buzzer (+) if needed
#endif
#ifdef GBUZZ
  pinMode(GBUZZ,OUTPUT);digitalWrite(GBUZZ, LOW);   // Supply Ground to Buzzer (-) if needed
#endif
  pinMode(SBUZZ,OUTPUT);        // Buzzer Signal Pin (S)
  pinMode(SS_RX_PIN, INPUT);    // XBee DOUT Pin
  pinMode(SS_TX_PIN, OUTPUT);   // XBee DIN Pin
    
  XBee.Timeout(WAIT_REPLY_MS);  // Set the Timeout for XBEE communications initialized in SetupMenu()
  IOSerial.begin(9600);         // Start UART Communications with the XBee->Module
  Serial.begin(9600);           // Start Serial Monitor for debug
  lcd.begin(16, 2);             // Start the LCD library
  SetupMenu();                  // Setup the Menu Items
  LCD_display();                // Display the main menu
}

/******************************************************************************************************************//**
 * @brief  Arduino Sketch Loop() routine
 * @remarks
 * - This function is called automatically over-and-over again by the Arduino
 * - Handles incoming XBee communications
 * - Handles button presses and LCD response updates
 * - Handles Menu iteratation during idle.
**********************************************************************************************************************/
void loop(){

//--- FORWARD SERIAL TO XBEE for XBEECONFIG --------------------
#if XBEECONFIG>0
  if ( IOSerial.available()>0 ) Serial.write(IOSerial.read());
  if ( Serial.available()>0 ) IOSerial.write(Serial.read());
#else
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

  XBee.Available();                                             // Check communications 
  
  #if NONBLOCKING>0                                             // Check for Non-Blocked Replies
    if ( PacketID != -1 ) {                                     // Assign Non-Blocking Get Items
      int Ret = XBee.GetReply(PacketID);
      if ( Ret != -1 ) {                                        // -- If reply value was available
        Menu[idx].Sub[MAIN].Value = Ret;                        // Assign received value
        Menu[idx].Sub[MAIN].State = VALID;                      // Validate received value
        CheckAlarms(idx);                                       // Check value for alarms
        PacketID = -1;                                          // Reset non-blocking packet
        LCD_display();                                          // Update Display with new value
      } 
      if ( millis() - wait_reply > WAIT_REPLY_MS ) {
        PacketID = -1;                                          // Time-out and ERR  
        LCD_display();                                          // Display the ERR
      }
    }
  #endif

  unsigned long clk = millis(); 
  Button bpress = last_bpress;                                  // Determine button pressed
  last_bpress = NONE;                                           // Release button press
  
  //--- Iterate Menu while Idle ---------------------------------------------------------------------
  if( bpress == NONE ) {
    if ( clk - last_bpress_millis > START_STATUS_ITERATE) bIterating = true;
    if ( bIterating && (( clk - last_iter_millis ) > ITERATE_EVERY) && !AlarmActive ) {
          SubIdx = MAIN;                                        // Switch to 'MAIN' menu items
          int i = idx;                                          // Mark current idx
          
          do {                                                  // Find the next menu item with an Alarm
            i++;                                                // Increment Menu Item
            if(i>MenuItemsIdx) i=0;                             // When reaching the end of Menu Items
            if(i == idx) break;                                 // break if we've made a full rotation
          } while (Menu[i].Sub[LOALARM].ID == 0 && Menu[i].Sub[HIALARM].ID == 0);
          
          idx=i;                                            // --- Now; set new 'idx' to the one with an Alarm ID
          PacketID = -1;                                    // Clear the previous PacketID; We're moving
          GetItem();                                        // Get the new value
          last_iter_millis=clk;                             // Record time for next iteration
          LCD_display();                                    // Update the display
    }

    ButtonCheck(analogRead(0));                            // Check for Left, Select button press

  //--- Process Button Press ------------------------------------------------------------------------
  } else {
    PacketID = -1;                                          // Clear Non-Blocked Packets
    noTone(SBUZZ);AlarmActive = false;                      // Turn off any alarms at button press
    bIterating = false;                                     // Stop Iterating Menu Items
    
    //------- ( SELECT ) -------
    if (bpress == SELECT) {
      SetItem();                                            // SET the Menu Item to its new value
      SubIdx = MAIN;                                        // Return to the MAIN items once a SET is done
      GetItem();                                            // Get the Items Value
    
    //------- (   UP   ) -------
    } else if (bpress == UP ) {
      
      if ( SubIdx == MAIN ) {                                             // 'UP' while viewing 'MAIN' Menu items
        idx--;if( idx<0 ) idx = MenuItemsIdx;                             // Decrement; Preform boundary check
        if ( Menu[idx].Sub[MAIN].State != VALID ) GetItem();              // Retreive Value

      } else {                                                            // 'UP' while viewing 'SET' or 'ALARM
        if ( Menu[idx].LastOptionIdx>0 ) {                                // Option Change
          Menu[idx].Sub[SubIdx].Value++;                                    // Move option down one
          if ( Menu[idx].Sub[SubIdx].Value > Menu[idx].LastOptionIdx ) {      // Boundary Check
            Menu[idx].Sub[SubIdx].Value = 0; }                                // Reset on Boundary
        } else if ( ButtonHeld < 5 ) {                                    // Increment Value
          ValueModify(idx,SubIdx,1);                                      // Change Value up 1
        } else {                                                          // Multi-Increment Value
          ValueModify(idx,SubIdx,5);                                      // Change Value up 10
        }
        if ( Menu[idx].Sub[SubIdx].Value > 1023 ) Menu[idx].Sub[SubIdx].Value = 1023; // Stop Value change at 1023
      }

    //------- (  DOWN  ) -------
    } else if (bpress == DOWN) {                                          
      
      if ( SubIdx == MAIN ) {                                                   // 'DOWN' while viewing 'MAIN' Menu items
        idx++;if ( idx>MenuItemsIdx ) idx=0;                                    // Increment; Preform boundary check
        if ( Menu[idx].Sub[MAIN].State != VALID ) GetItem();                    // Retreive Value
      
      } else {                                                                  // 'DOWN' while viewing 'SET' or 'ALARM
        if ( Menu[idx].LastOptionIdx>0 ) {                                      // Option Change
          Menu[idx].Sub[SubIdx].Value--;                                          // Move Option up one
          if ( Menu[idx].Sub[SubIdx].Value < 0) {                                   // Boundary Check
            Menu[idx].Sub[SubIdx].Value = Menu[idx].LastOptionIdx; }                // Reset on Boundary
        } else if ( ButtonHeld < 5 ) {                                          // Decrement Value
          ValueModify(idx,SubIdx,-1);                                             // Change Value down 1
        } else {                                                                // Multi-Decrement Value
          ValueModify(idx,SubIdx,-5);                                            // Change Value down 10
        }
        if ( Menu[idx].Sub[SubIdx].Value < 0 ) Menu[idx].Sub[SubIdx].Value = 0; // Stop Value change at 0
      }
    
    //------- (  RIGHT ) -------
    } else if (bpress == RIGHT) {
      SubIdx++;
      if ( SubIdx == SET && Menu[idx].Sub[SET].State != SETTABLE ) SubIdx++;    // Skip SET if not SETTABLE
      if ( SubIdx == LOALARM && Menu[idx].Sub[LOALARM].ID == 0 ) SubIdx++;   // Skip LOALARM if not Identified
      if ( SubIdx == HIALARM && Menu[idx].Sub[HIALARM].ID == 0 ) SubIdx++;   // Skip HIALARM if not Identified
      if ( SubIdx > HIALARM ) SubIdx = MAIN;
      if ( Menu[idx].Sub[MAIN].State == VALID ) {
        if ( SubIdx == SET && Menu[idx].Sub[SET].Value < 0 ) Menu[idx].Sub[SET].Value = Menu[idx].Sub[MAIN].Value; // Make SET value MAIN value instead of ERR
      } else {
        GetItem();
      }
    
    //------- (  LEFT  ) -------  
    } else if (bpress == LEFT) {
      SubIdx--;if (SubIdx<0) SubIdx=0;
      if ( SubIdx == SET && Menu[idx].Sub[SET].State != SETTABLE ) SubIdx--;    // Skip SET if not SETTABLE
      if ( SubIdx == LOALARM && Menu[idx].Sub[LOALARM].ID == 0 ) SubIdx--;   // Skip LOALARM if not identified
      if ( SubIdx == HIALARM && Menu[idx].Sub[HIALARM].ID == 0 ) SubIdx--;   // Skip HIALARM if not identified
      if ( SubIdx < MAIN ) SubIdx = MAIN;
      if ( SubIdx == MAIN && Menu[idx].Sub[MAIN].State != VALID ) GetItem();
    }
    
    LCD_display();                                      // Update Display after button press
  }
  
#endif
}


