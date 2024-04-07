#include <RPi_Pico_TimerInterrupt.h>
#include <EEPROM.h>
#include <tusb.h>

// comment this out if 7-segment displays and rotary encoder are not installed
// (frees up GPIO pins 19, 20, 21, 22, 26, 27, 28)
#define HAVE_DISPLAY

// defining BUFFER_SERIAL will transmit captured data to the PC much quicker
// (see BUFFER_SERIAL section below). However, some terminal programs, e.g.
// as TeraTerm and Putty on Windows, may occasionally show corrupted data.
// Others (like the Arduino serial monitor) do not have this issue.
//#define BUFFER_SERIAL

// commenting this out will configure PIN_CLR as an OUTPUT
// => make sure it is NOT connected to the bus in this state
//#define DEBUG

// pin 0-7:  data bus
// pin 8-15: address bus
#define PIN_INPUT    16
#define PIN_OUTPUT   17
#define PIN_CLR      18

#ifdef HAVE_DISPLAY
#define PIN_SR_CLK   19
#define PIN_SR_LATCH 20
#define PIN_SR_ENAB  21
#define PIN_SR_DATA  22
#define PIN_ROT_BTN  26
#define PIN_ROT_B    27
#define PIN_ROT_A    28
#endif

#define MASK_INOUT  (bit(PIN_INPUT) | bit(PIN_OUTPUT))

#define PD_OUTPUT   0x0000
#define PD_INPUT    0x0100
#define PD_OFF      0x8000

#define ST_IDLE      0
#define ST_CONFIG_IO 1
#define ST_CONFIG_HI 2
#define ST_CONFIG_LO 3

#define FLT_OFF       0 // everything recorded
#define FLT_INCLUSIVE 1 // record port accesses matching a filter settings
#define FLT_EXCLUSIVE 2 // record port accesses NOT matching any filter settings
#define FLT_ALL       3 // nothing recorded


bool haveSerial = false;
int markPort = -1, configPort = -1;
uint8_t ports[512], configDigits[2], configBlinkDigit;
uint32_t cycleTime;

// configuration data (serial output)
int outputLineLength = 16;
bool outputPrintASCII = true, outputSuppressRepeats = true;
volatile uint32_t filterType = FLT_EXCLUSIVE;
volatile uint32_t filter[4] = {0x00, 0x06, 0x10, 0x12};

// configuration data (7-segment displays)
uint16_t portDisplay[4];



// -------------------------------------------------------------------------------------------------
// bus data queue
// -------------------------------------------------------------------------------------------------

#define BUSQUEUE_SIZE 45000 // requires 4*BUSQUEUE_SIZE bytes of RAM
volatile uint32_t busqueue[BUSQUEUE_SIZE], busqueue_start, busqueue_end;
spin_lock_t* busqueue_lock = NULL;
volatile uint8_t overflow = 0;


void __not_in_flash_func(busqueue_add)(uint32_t data)
{
  // busqueue_add is only called from the second core on which
  // interrupts are permanently disabled so we can safely use
  // the spin_*_unsafe functions which do not deal with interrupts
  spin_lock_unsafe_blocking(busqueue_lock);  
  if( !overflow )
    {
      uint32_t new_end = busqueue_end+1;
      if( new_end==BUSQUEUE_SIZE ) new_end = 0;
      
      if( new_end==busqueue_start )
        {
          overflow = 1; 
          gpio_set_mask(bit(25)); // turn the on-board LED on
        }
      else
        {
          busqueue[busqueue_end] = data;
          busqueue_end = new_end;
        }
    }
  spin_unlock_unsafe(busqueue_lock);
}


bool __no_inline_not_in_flash_func(busqueue_get)(uint32_t *data)
{
  uint32_t interrupts = spin_lock_blocking(busqueue_lock);  
  bool ok = busqueue_end!=busqueue_start;
  if( ok )
    {
      *data = busqueue[busqueue_start];
      if( ++busqueue_start==BUSQUEUE_SIZE ) busqueue_start = 0;
    }
  else if( overflow )
    { 
      // buffer is empty => clear overflow mode
      overflow = 0; 
      gpio_clr_mask(bit(25)); // turn the on-board LED off
    }

  spin_unlock(busqueue_lock, interrupts);
  return ok;
}


void busqueue_clear()
{
  uint32_t interrupts = spin_lock_blocking(busqueue_lock);  
  busqueue_start = busqueue_end;
  overflow = 0;
  gpio_clr_mask(bit(25));  // turn the on-board LED off
  spin_unlock(busqueue_lock, interrupts);
}


// -------------------------------------------------------------------------------------------------
// data capture process (running on second core
// -------------------------------------------------------------------------------------------------


void __no_inline_not_in_flash_func(core2)()
{
  noInterrupts();
  uint32_t prevData   = gpio_get_all();
  uint32_t prevSample = 0xFFFFFFFF;
  uint32_t repeat     = 0;
  uint32_t inactive   = 0;
  while(1) 
    {
      // one cycle of this loop (while not detecting a sample) is about 200ns (at 133MHz)
#if 0 && defined(DEBUG)
      gpio_xor_mask(bit(PIN_CLR));
#endif
      uint32_t data = gpio_get_all();
      if( (data & MASK_INOUT) != (prevData & MASK_INOUT) )
        {
          // a change was seen on PIN_INPUT or PIN_OUTPUT
          // this may have been just noise, induced by some other signal changing
          // => wait a short time and read the inputs again
          asm volatile ("nop\n nop\n nop\n nop\n");
          uint32_t data2 = gpio_get_all();
          if( (data2 & MASK_INOUT) == (data & MASK_INOUT) )
            {
              // still the same IN/OUT signals => this is legitimate
              data = data2 & (MASK_INOUT | 0xFFFF);
            }
          else
            {
              // not the same change => start over
              continue;
            }
        }

      uint32_t sample = 0xFFFFFFFF;
      // sample INP on HIGH->LOW edge, OUT on LOW->HIGH edge
      if( (prevData & bit(PIN_INPUT)) && !(data & bit(PIN_INPUT)) ) 
        sample = (data & 0xFFFF) | bit(PIN_INPUT);
      else if( !(prevData & bit(PIN_OUTPUT)) && (data & bit(PIN_OUTPUT)) )
        sample = (data & 0xFFFF);

      if( sample!=0xFFFFFFFF )
        {
          // update port array (for LED displays)
          uint32_t portNum = (sample>>8) & 0x1FF;
          ports[portNum] = data&0xFF;

          if( haveSerial )
            {
              if( sample==prevSample && repeat<127 )
                {
                  repeat++;
                  inactive = 0;
                }
              else if( (filterType == FLT_OFF) 
                       ||
                       (filterType == FLT_INCLUSIVE && (portNum==filter[0] || portNum==filter[1] || portNum==filter[2] || portNum==filter[3]) )
                       ||
                       (filterType == FLT_EXCLUSIVE && (portNum!=filter[0] && portNum!=filter[1] && portNum!=filter[2] && portNum!=filter[3]) ) )
                {
                  // we have a new (different) sample
                  if( prevSample!=0xFFFFFFFF )
                    {
                      // if we had a previous sample, store it now
                      busqueue_add(prevSample | (repeat << 17));
                    }

                  prevSample = sample;
                  repeat = 1;
                  inactive = 0;
                }
            }
        }

      if( prevSample!=0xFFFFFFFF && (++inactive)>=5000 )
        {
          // one cycle of the outer loop (while not detecting a sample) is about 200ns
          // => nothing was sampled in about 5000*200ns=1ms => store sample
          busqueue_add(prevSample | (repeat << 17));
          prevSample = 0xFFFFFFFF;
          inactive = 0;
        }

      prevData = data;
    }
}


// -------------------------------------------------------------------------------------------------
// 7-segment display handling (called via timer interrupt)
// -------------------------------------------------------------------------------------------------

#ifdef HAVE_DISPLAY

// segment order: PABCDEFG (P=decimal point), digits 0123456789AbCdEFnu-
static byte segments[19] = {0b01111110, 0b00110000, 0b01101101, 0b01111001, 
                            0b00110011, 0b01011011, 0b01011111, 0b01110000, 
                            0b01111111, 0b01111011, 0b01110111, 0b00011111, 
                            0b01001110, 0b00111101, 0b01001111, 0b01000111,
                            0b00010101, 0b00011100, 0b00000001};


void __no_inline_not_in_flash_func(shiftOutDigit)(uint8_t segments, uint8_t places)
{
  // the 74HC595 shift register reads data on the clock LOW->HIGH edge
  // (this function takes about 2us total at 133MHz)
  gpio_clr_mask(bit(PIN_SR_LATCH));
  uint32_t data = (places<<8) | (255-segments);
  for(int i=0; i<16; i++)
    {
      gpio_clr_mask(bit(PIN_SR_CLK));
      if( data & 0x8000 ) gpio_set_mask(bit(PIN_SR_DATA)); else gpio_clr_mask(bit(PIN_SR_DATA));
      asm volatile ("nop\n");
      gpio_set_mask(bit(PIN_SR_CLK));
      asm volatile ("nop\n");
      data <<= 1;
    }
  gpio_clr_mask(bit(PIN_SR_CLK));
  gpio_set_mask(bit(PIN_SR_LATCH));
}


void digitOff(int place, bool decimalPoint = false)
{
  shiftOutDigit(decimalPoint ? 0x80 : 0x00, 1<<place);
}

void displayDigit(int place, int digit, bool decimalPoint)
{
  shiftOutDigit(segments[digit] | (decimalPoint ? 0x80 : 0x00), 1<<place);
}


bool updateLEDs(struct repeating_timer *t)
{
  // called every 500us via timer interrupt
  static uint8_t n = 0;
  uint16_t port = portDisplay[n/2];

  if( n/2 == configPort )
    {
      if( (millis() & 256)==0 && (((n&1)+1)&configBlinkDigit) )
        digitOff(n);
      else
        displayDigit(n, configDigits[n&1], false);
    }
  else if( (port & PD_OFF)==0 )
    {
      uint16_t data = ports[portDisplay[n/2]&511];
      displayDigit(n, n&1 ? (data&15) : (data/16), (n&1)==0 && n/2==markPort);
    }
  else
    digitOff(n, (n&1)==0 && n/2==markPort);
  
  n = (n+1)&7;
  return true;
}

#endif

// -------------------------------------------------------------------------------------------------
// configuration data save/load
// -------------------------------------------------------------------------------------------------


size_t configSize()
{
  return (sizeof(portDisplay) + 
          sizeof(filterType) + 
          sizeof(filter) + 
          sizeof(outputSuppressRepeats) +  
          sizeof(outputPrintASCII) + 
          sizeof(outputLineLength));
}


void saveConfig()
{
  uint8_t *p = EEPROM.getDataPtr();
  memcpy(p, (void *) portDisplay, sizeof(portDisplay)); p += sizeof(portDisplay);
  memcpy(p, (void *) &filterType, sizeof(filterType)); p += sizeof(filterType);
  memcpy(p, (void *) filter, sizeof(filter)); p += sizeof(filter);
  memcpy(p, (void *) &outputSuppressRepeats, sizeof(outputSuppressRepeats)); p += sizeof(outputSuppressRepeats);
  memcpy(p, (void *) &outputPrintASCII, sizeof(outputPrintASCII)); p += sizeof(outputPrintASCII);
  memcpy(p, (void *) &outputLineLength, sizeof(outputLineLength)); p += sizeof(outputLineLength);

#ifdef HAVE_DISPLAY
  // EEPROM.commit() disables interrupts => turn off LEDs during commit
  digitalWrite(PIN_SR_ENAB, HIGH);
  EEPROM.commit();
  digitalWrite(PIN_SR_ENAB, LOW);
#else
  EEPROM.commit();
#endif
}


void loadConfig()
{
  uint8_t *p = EEPROM.getDataPtr();
  if( strncmp((char *) p+configSize(), "APM", 3)!=0 )
    {
      filterType = FLT_OFF;
      for(int i=0; i<4; i++) { portDisplay[i]=PD_OFF; filter[i] = PD_OFF; }
      memcpy(p+configSize(), "APM", 3);
      saveConfig();
    }
  else
    {
      memcpy((void *) portDisplay, p, sizeof(portDisplay)); p+=sizeof(portDisplay);
      memcpy((void *) &filterType, p, sizeof(filterType)); p+=sizeof(filterType);
      memcpy((void *) filter, p, sizeof(filter)); p+=sizeof(filter);
      memcpy((void *) &outputSuppressRepeats, p, sizeof(outputSuppressRepeats)); p+=sizeof(outputSuppressRepeats);
      memcpy((void *) &outputPrintASCII, p, sizeof(outputPrintASCII)); p+=sizeof(outputPrintASCII);
      memcpy((void *) &outputLineLength, p, sizeof(outputLineLength)); p+=sizeof(outputLineLength);
    }
}
 

// -------------------------------------------------------------------------------------------------
// 7-segment display configuration
// -------------------------------------------------------------------------------------------------

#ifdef HAVE_DISPLAY

int readRotaryEncoder()
{
  static bool pinAstate = true;
  bool pinA, pinB;
  
  pinA = digitalRead(PIN_ROT_A);
  if( pinA != pinAstate )
    {
      pinB = digitalRead(PIN_ROT_B);
      delay(2);
      pinA = digitalRead(PIN_ROT_A);
      if( pinA != pinAstate )
        {
          pinAstate = pinA;
          if( !pinA ) return pinB ? 1 : -1;
        }
    }

  return 0;
}


bool buttonClicked()
{
  static bool btnState = true;
  bool btn;
  
  btn = digitalRead(PIN_ROT_BTN);
  if( btn != btnState )
    {
      delay(2);
      if( btn != btnState ) 
        {
          btnState = btn;
          return !btn;
        }
    }

  return false;
}


void handleLEDConfig()
{
  int rot;
  static unsigned long timeout = 0;
  static int state = ST_IDLE;
  static uint16_t configData;

  if( timeout>0 && cycleTime>timeout )
    {
      configPort = -1;
      markPort = -1;
      state = ST_IDLE;
      timeout = 0;
    }

  switch( state )
    {
    case ST_IDLE:
      {
        rot = readRotaryEncoder();
        if( rot!=0 )
          {
            if( markPort>=0 )
              {
                markPort += rot;
                if( markPort > 3 )
                  markPort = 0;
                else if( markPort < 0 )
                  markPort = 3;
              }
            else if( rot>0 )
              markPort = 0;
            else if( rot<0 )
              markPort = 3;

            timeout = cycleTime+2000;
          }

        if( markPort>=0 && buttonClicked() )
          {
            state = ST_CONFIG_IO;
            configPort = markPort;
            timeout = cycleTime+5000;
            configData = portDisplay[configPort];
            configBlinkDigit = 3;
          }

        break;
      }

    case ST_CONFIG_IO:
      {
        rot = readRotaryEncoder();
        if( rot!=0 )
          {
            uint16_t d = configData & (PD_INPUT|PD_OUTPUT|PD_OFF);
            if( (d==PD_INPUT && rot>0) || (d==PD_OFF && rot<0) )
              configData = (configData & 255) | PD_OUTPUT;
            else if( (d==PD_OUTPUT && rot>0) || (d==PD_INPUT && rot<0) )
              configData = (configData & 255) | PD_OFF;
            else if( (d==PD_OFF && rot>0) || (d==PD_OUTPUT && rot<0) )
              configData = (configData & 255) | PD_INPUT;

            timeout = cycleTime+5000;
          }

        if( buttonClicked() )
          {
            if( configData & PD_OFF )
              {
                portDisplay[configPort] = configData;
                saveConfig();
                configPort = -1;
                state = ST_IDLE;
              }
            else
              {
                state = ST_CONFIG_HI;
                configBlinkDigit = 1;
              }
          }
        else
          {
            if( configData & PD_OFF )
              { configDigits[0]=18; configDigits[1]=18; } // --
            else if( configData & PD_INPUT )
              { configDigits[0]= 1; configDigits[1]=16; } // In
            else
              { configDigits[0]= 0; configDigits[1]=17; } // Ou
          }

        break;
      }

    case ST_CONFIG_HI:
      {
        rot = readRotaryEncoder();
        if( rot!=0 )
          {
            configData = ((configData + 16*rot) & 255) | (configData & PD_INPUT);
            timeout = cycleTime+5000;
          }

        if( buttonClicked() )
          {
            state = ST_CONFIG_LO;
            configBlinkDigit = 2;
          }
        else
          {
            configDigits[0] = (configData/16)&15;
            configDigits[1] = configData&15;
          }

        break;
      }
      
    case ST_CONFIG_LO:
      {
        rot = readRotaryEncoder();
        if( rot!=0 )
          {
            configData = ((configData + rot) & 255) | (configData & PD_INPUT);
            timeout = cycleTime+5000;
          }

        if( buttonClicked() )
          {
            portDisplay[configPort] = configData;
            saveConfig();
            configPort = -1;
            state = ST_IDLE;
          }
        else
          {
            configDigits[0] = (configData/16)&15;
            configDigits[1] = configData&15;
          }

        break;
      }
    }
}

#endif


// -------------------------------------------------------------------------------------------------
// serial output configuration (filter/output format)
// -------------------------------------------------------------------------------------------------


static String read_user_cmd()
{
  uint32_t savedFilterType = filterType;
  filterType = FLT_ALL;

  bool first = true;
  String s;
  do
    {
      int i = Serial.read();

      if( (i==13 || i==10) && !first )
        break;
      else if( i==27 )
        { s = ""; break; }
      else if( i==8  )
        { 
          if( s.length()>0 )
            { Serial.write(8); Serial.write(' '); Serial.write(8); s = s.substring(0, s.length()-1); }
        }
      else if( isprint(i) )
        { s = s + String((char )i); Serial.write(i); first = false; }
    }
  while(true);

  filterType = savedFilterType;
  return s;
}


void printFilterType(uint32_t t)
{
  switch( t )
    {
    case FLT_OFF:       Serial.println("Filter is off (everything recorded)"); break;
    case FLT_INCLUSIVE: Serial.println("Filter is inclusive (matching operations recorded)"); break;
    case FLT_EXCLUSIVE: Serial.println("Filter is exclusive (matching operations NOT recorded)"); break;
    case FLT_ALL:       Serial.println("Filter is all (nothing recorded)"); break;
    }
}


void printFilter(int i, uint32_t f)
{
  if( f & PD_OFF )
    Serial.printf("Filter %i: disabled\r\n", i+1);
  else
    Serial.printf("Filter %i: Match %s operations on port %i (%02Xh)\r\n", 
                  i+1, (f & PD_INPUT) ? "input" : "output", f&0xFF, f&0xFF);
}


void printOutputFormat()
{
  Serial.print("Output format is: ");
  if( outputLineLength < 0 )
    Serial.println("raw");
  else if( outputLineLength==0 )
    Serial.println("binary");
  else
    {
      Serial.printf("hexdump, %i bytes per line", outputLineLength);
      if( outputPrintASCII ) Serial.print(", include ASCII output");
      if( outputSuppressRepeats ) Serial.print(", suppress repeated lines");
      Serial.println();
    }
}


void printFilterSettings()
{
  printFilterType(filterType);
  if( filterType==FLT_INCLUSIVE || filterType==FLT_EXCLUSIVE )
    for(int i=0; i<4; i++)
      printFilter(i, filter[i]);
}


void serialConfig()
{
  String tokens[10];

  if( Serial.peek()==27 || Serial.peek()==10 || Serial.peek()==13 ) 
    { 
      Serial.println();
      printFilterSettings(); 
      printOutputFormat();
      Serial.read(); 
    }

  while( true )
    {
      int numTokens = 0;
      Serial.print("\r\nCommand: ");
      String cmdline = read_user_cmd();
      cmdline.trim();
      cmdline.toLowerCase();
      Serial.println();
      if( cmdline=="" ) break;

      while( cmdline.length()>0 )
        {
          int i = cmdline.indexOf(' ');
          if( i<0 )
            {
              tokens[numTokens++] = cmdline;
              cmdline="";
            }
          else
            {
              tokens[numTokens++] = cmdline.substring(0, i);
              cmdline = cmdline.substring(i);
              cmdline.trim();
            }
        }

      if( numTokens==1 && (tokens[0]=="e" || tokens[0]=="x" || tokens[0]=="exit") )
        break;
      else if( numTokens==1 && (tokens[0]=="o" || tokens[0]=="output") )
        printOutputFormat(); 
      else if( numTokens>1 && (tokens[0]=="o" || tokens[0]=="output") )
        {
          bool ok = true;
          if( tokens[1] == "r" || tokens[1]=="raw" )
            outputLineLength = -1;
          else if( tokens[1] == "b" || tokens[1]=="bin" )
            outputLineLength = 0;
          else if( numTokens>=2 && tokens[1] == "h" || tokens[1]=="hex" )
            {
              int numBytes;
              if( numTokens==2 )
                outputLineLength = 16;
              else if( numTokens>2 && sscanf(tokens[2].c_str(), "%i", &numBytes)==1 && numBytes>0 && numBytes<256 )
                outputLineLength = numBytes;
              else
                ok = false;
            }
          else if( numTokens>=2 && (tokens[1] == "a" || tokens[1]=="ascii") )
            {
              if( numTokens==2 )
                outputPrintASCII = !outputPrintASCII;
              else if( numTokens>1 && (tokens[2]=="1" || tokens[2]=="on") )
                outputPrintASCII = true;
              else if( numTokens>1 && (tokens[2]=="0" || tokens[2]=="off" ) )
                outputPrintASCII = false;
              else
                Serial.println("Invalid setting for ascii output");
            }
          else if( numTokens>=2 && (tokens[1] == "s" || tokens[1]=="suppress") )
            {
              if( numTokens==2 )
                outputSuppressRepeats = !outputSuppressRepeats;
              else if( numTokens>1 && (tokens[2]=="1" || tokens[2]=="on") )
                outputSuppressRepeats = true;
              else if( numTokens>1 && (tokens[2]=="0" || tokens[2]=="off" ) )
                outputSuppressRepeats = false;
              else
                Serial.println("Invalid setting for output repeat suppression");
            }
          else
            ok = false;
          
          if( ok )
            { saveConfig(); printOutputFormat(); }
          else
            Serial.println("Invalid output format");
        }
      else if( numTokens>=1 && (tokens[0]=="f" || tokens[0]=="filter") )
        {
          if( numTokens==1 )
            {
              Serial.println("Filter settings:");
              printFilterSettings();
            }
          else if( numTokens==2 )
            {
              bool ok = true;
              if( tokens[1]=="o" || tokens[1]=="off" )
                filterType = FLT_OFF;
              else if( tokens[1]=="i" || tokens[1]=="inclusive" )
                filterType = FLT_INCLUSIVE;
              else if( tokens[1]=="e" || tokens[1]=="exclusive" )
                filterType = FLT_EXCLUSIVE;
              else if( tokens[1]=="a" || tokens[1]=="all" )
                filterType = FLT_ALL; 
              else
                ok = false;

              if( ok )
                {
                  saveConfig();
                  printFilterSettings();
                }
              else
                Serial.println("Invalid filter type, expected one of: o,i,e,a,off,inclusive,exclusive,all");
            }
          else if( numTokens==3 )
            {
              bool ok = true;
              int i = 0;
              uint32_t f = 0;

              if( sscanf(tokens[1].c_str(), "%i", &i)!=1 || i<1 || i>4 )
                ok = false;

              if( tokens[2][0]=='i' )
                f |= PD_INPUT;
              else if( tokens[2][0]=='o' )
                f |= PD_OUTPUT;
              else if( tokens[2][0]=='-' )
                f |= PD_OFF;
              else
                ok = false;

              if( tokens[2].length()==4 && tokens[2][3]=='h' )
                {
                  uint32_t p;
                  if( sscanf(tokens[2].c_str()+1, "%x", &p)==1 )
                    f |= p;
                  else
                    ok = false;
                }
              else if( tokens[2].length()>1 )
                {
                  uint32_t p;
                  if( sscanf(tokens[2].c_str()+1, "%u", &p)==1 )
                    f |= p;
                  else
                    ok = false;
                }
              else if( tokens[2][0]!='-' )
                ok = false;

              if( ok )
                {
                  printFilter(i-1, f);
                  filter[i-1] = f;
                  saveConfig();
                }
              else
                Serial.println("Invalid filter setting, expected: filter [1-4] [iN|iXXh|oN|oXXh|-]");
            }
        }
      else
        {
          Serial.println("Commands:");
          Serial.println("  f|filter                print current filter settings");
          Serial.println("  f|filter o|off          disable filtering (record everything)");
          Serial.println("  f|filter i|inclusive    record all operations matching filters 1-4");
          Serial.println("  f|filter e|exclusive    record all operations NOT matching filters 1-4");
          Serial.println("  f|filter a|all          filter everything (record nothing)");
          Serial.println("  f|filter n -            disable filter #n");
          Serial.println("  f|filter n iN           set filter #n to 'input port (decimal) N'");
          Serial.println("  f|filter n iXXh         set filter #n to 'input port (hex) XX'");
          Serial.println("  f|filter n oN           set filter #n to 'output port (decimal) N'");
          Serial.println("  f|filter n oXXh         set filter #n to 'output port (hex) XX'");
          Serial.println("  o|output h|hex [n]      produce dexdump output (n is number of bytes per line, default 16)");
          Serial.println("  o|output a|ascii [0|on|1|off] show ASCII representation in hexdump output");
          Serial.println("  o|output s|suppress [0|on|1|off] suppress repeated lines of data in hexdump output");
          Serial.println("  o|output r|raw          produce raw output: I|Oppvvrr");
          Serial.println("  o|output b|bin          produce binary output (3 bytes/operation)");
          Serial.println("  e|x|exit                exit menu");
        }
    }

  Serial.println("exit"); 
  Serial.println();
}


// -------------------------------------------------------------------------------------------------
// output captured data over serial
// -------------------------------------------------------------------------------------------------

uint32_t serial_check_receive = 0;
const char *hex = "0123456789ABCDEF";

#ifndef BUFFER_SERIAL

// This implementation is fast enough for monitoring all real software I have observed.
// Use of BUFFER_SERIAL is only necessary if trying to capture very tight loops (see below).
void serialBufferFlush() {}
void serialBufferedWrite(uint8_t data) { Serial.write(data); }

#else

/* Calling Serial.write() to report captured data is not very efficient and can lead
   to capture buffer overflows in VERY tight loops (see example below). 
   This implementation buffers serial data util a full USB bulk data package is collected.
   HOWEVER, this comes at a price: For some reason TeraTerm and Putty have issues
   with data arriving this quickly and occasionally show some corrupted output.
   I have verified via WireShark and direct USB capture that all data actually DOES arrive
   at the PC, it is just not shown properly by Putty/TeraTerm.

   Example code:
     0000: IN  30    ; 10 cycles
     0002: IN  31    ; 10 cycles
     0004: IN  30    ; 10 cycles
     0006: IN  31    ; 10 cycles
     0008: JMP 0000  ; 10 cycles

   The above example produces 4 input operations (i.e. 12 bytes of data in "output binary" 
   mode) every 50 cycles, i.e. 25us => 480kb of data per second

   To capture all data in the above example:
   - compile with "#define BUFFER_SERIAL" enabled
   - must use "output binary" mode (any other mode will use too many characters per
     operation and saturate the USB full speed data rate)
   - must use a VERY fast receiving program on the PC end (e.g. TeraTerm is too slow)
*/

#define SERIAL_BUFFER_INTERVAL 20
#define SERIAL_BUFFER_SIZE 512
uint32_t serial_bufptr = 0;
uint32_t serial_check_send = 0;
uint8_t serial_buffer[SERIAL_BUFFER_SIZE];

void serialBufferFlush()
{
  if( serial_bufptr>0 )
    {
      Serial.write(serial_buffer, serial_bufptr);
      serial_bufptr = 0;
    }

  serial_check_send = cycleTime + SERIAL_BUFFER_INTERVAL;
}


void serialBufferedWrite(uint8_t data)
{
  if( serial_bufptr == SERIAL_BUFFER_SIZE ) 
    {
      // buffer is full => must send it now (will block until sent)
      Serial.write(serial_buffer, SERIAL_BUFFER_SIZE);
      serial_bufptr = 0;
    }
  else if( serial_bufptr>=64 && (serial_bufptr&7)==0 && Serial.availableForWrite()>=64 )
    {
#if 0
      // send a full USB bulk package of data
      Serial.write(serial_buffer, 64);
      memmove(serial_buffer, serial_buffer+64, serial_bufptr-64);
      serial_bufptr -= 64;
#else
      tud_task();
      if( tud_cdc_write_available()>=64 )
        {
          tud_cdc_write(serial_buffer, 64);
          serial_bufptr -= 64;
          memmove(serial_buffer, serial_buffer+64, serial_bufptr);
        }
#endif
    }
  
  serial_check_send = cycleTime + SERIAL_BUFFER_INTERVAL;
  serial_buffer[serial_bufptr++] = data;
}


#endif

void printHexLineHeader(uint32_t data)
{
  serialBufferedWrite((data & bit(PIN_INPUT)) ? 'I' : 'O');
  serialBufferedWrite(hex[(data>>12)&0x0F]);
  serialBufferedWrite(hex[(data>>8)&0x0F]);
  serialBufferedWrite(':');
}


void printHexLineData(uint32_t data)
{
  serialBufferedWrite(' ');
  serialBufferedWrite(hex[(data>>4)&0x0F]);
  serialBufferedWrite(hex[(data>>0)&0x0F]);
}


void printASCIILineData(int numData, uint8_t *data)
{
  if( numData<=8 && outputLineLength==16 ) serialBufferedWrite(' ');
  for(int i=numData; i<17; i++) { serialBufferedWrite(' '); serialBufferedWrite(' '); serialBufferedWrite(' '); }
  for(int i=0; i<numData; i++) 
    {
      serialBufferedWrite(isprint(data[i]) ? data[i] : '.');
      if( i==7 && outputLineLength==16 ) serialBufferedWrite(' ');
    }
}


void handleSerialOutput()
{
  bool enteredConfig = false;

  // calling Serial.available() is expensive (time-wise) so we only check every 100ms
  if( cycleTime>serial_check_receive )
    {
      haveSerial = Serial;
      if( haveSerial && Serial.available()>0 ) 
        { 
          serialBufferFlush(); 
          busqueue_clear();
          serialConfig();
          busqueue_clear(); 
          enteredConfig = true; 
          cycleTime = millis();
          serialBufferFlush(); 
        }

      serial_check_receive = cycleTime + 100;
    }

#ifdef BUFFER_SERIAL
  // flush out data buffered for sending
  if( cycleTime>serial_check_send ) serialBufferFlush();
#endif

  if( outputLineLength<=0 ) 
    {
      uint32_t data;

      if( busqueue_get(&data) )
        {
          if( outputLineLength==0 )
            {
              // binary data output (3 bytes)
              // byte 1 : nnnnnnni (nnnnnnn:count, i:1=input,0=output)
              // byte 2 : port number
              // byte 3 : data byte

              // report capture buffer overflow
              // (note that 0/0/0 is not a generally valid package since the data count
              // in byte 1 of a valid package is greater than zero)
              if( overflow==1 )
                {
                  overflow = 2;
                  serialBufferedWrite(0);
                  serialBufferedWrite(0);
                  serialBufferedWrite(0);
                }
              
              serialBufferedWrite((data>>16)&0xFF);
              serialBufferedWrite((data>>8)&0xFF);
              serialBufferedWrite((data>>0)&0xFF);
            }
          else
            {
              // raw ASCII data output, format TXXYYZZ
              // where  T: either I (input) or O (output)
              //       XX: port number (hex)
              //       YY: data byte (hex)
              //       ZZ: count of same data

              // report capture buffer overflow
              if( overflow==1 )
                {
                  overflow = 2;
                  serialBufferFlush();
                  Serial.println("\r\n[CAPTURE BUFFER OVERFLOW]");
                }

              serialBufferedWrite((data & bit(PIN_INPUT)) ? 'I' : 'O');
              serialBufferedWrite(hex[(data>>12)&0x0F]);
              serialBufferedWrite(hex[(data>>8)&0x0F]);
              serialBufferedWrite(hex[(data>>4)&0x0F]);
              serialBufferedWrite(hex[(data>>0)&0x0F]);
              serialBufferedWrite(hex[(data>>21)&0x0F]);
              serialBufferedWrite(hex[(data>>17)&0x0F]);
              serialBufferedWrite(13); serialBufferedWrite(10);
            }
        }
    }
  else
    {
      // hexdump data output
      static uint32_t data, count = 0;
      static uint32_t prevData = 0xFFFFFFFF, prevPort = 0xFFFFFFFF;
      static int numData=0, numSameData=0;
      static int32_t numSkippedLines = -1;
      static uint8_t lineBuf[256];
      
      // report capture buffer overflow
      if( overflow==1 )
        {
          overflow = 2;
          if( outputPrintASCII ) printASCIILineData(numData, lineBuf);
          numData = 0;
          serialBufferFlush();
          Serial.println("\r\n[CAPTURE BUFFER OVERFLOW]");
        }

      if( enteredConfig )
        {
          // we entered configuration mode above => start fresh
          prevPort = 0xFFFFFFFF; 
          prevData = 0xFFFFFFFF; 
          numData = 0; 
          numSameData = 0;
          numSkippedLines = -1;
          count = 0;
        }
      
      // if we've finished processing a data sample then get next sample
      if( count==0 && busqueue_get(&data) )
        {
          count = data >> 17;
          data  = data & 0x1FFFF;
        }

      if( count > 0 )
        {
          count--;

          if( data!=prevData && numSkippedLines>=0 )
            {
              // port or data value has changed and we are in line-skip mode
              if( numSkippedLines>0 )
                {
                  // there were actual skipped lines => print count
                  char buf[20];
                  sprintf(buf, " (%u)\r\n", numSkippedLines);
                  for(char *c=buf; *c; c++) serialBufferedWrite(*c);
                }
              else
                { 
                  // no lines were actually skipped => remove "..." (was already printed)
                  serialBufferedWrite(8); serialBufferedWrite(8); serialBufferedWrite(8); serialBufferedWrite(13); 
                }

              if( numSameData>0 )
                {
                  // print buffered data
                  printHexLineHeader(prevData);
                  for(numData=0; numData<numSameData; numData++)
                    {
                      if( numData==8 && outputLineLength==16 ) serialBufferedWrite(' ');
                      printHexLineData(lineBuf[numData]);
                    }
                }

              if( numData==outputLineLength )
                {
                  // printed a full line of data => finish it
                  if( outputPrintASCII ) printASCIILineData(numData, lineBuf);
                  serialBufferedWrite(13); serialBufferedWrite(10);
                  numData = 0;
                }
              
              // no longer in line-skip mode
              numSameData = 0;
              numSkippedLines = -1;
            }
          
          uint32_t port = (data >> 8) & 0x1FF;
          if( port!=prevPort )
            {
              // port has changed => finish the line
              if( numData>0 )
                {
                  if( outputPrintASCII ) printASCIILineData(numData, lineBuf);
                  serialBufferedWrite(13); serialBufferedWrite(10);
                }
              
              numData = 0;
              numSameData = 0;
              prevPort = port;
            }
          
          if( numSkippedLines>=0 )
            {
              // we are in line-skip mode
              // (we know that data==prevData, otherwise we would have exited line-skip mode above)
              if( numSameData==0 && numSkippedLines==0 ) 
                {
                  // first byte of first (potentially) skipped line => print "..."
                  serialBufferedWrite('.');serialBufferedWrite('.');serialBufferedWrite('.');
                }

              // remember data so we can print it later if necessary
              lineBuf[numSameData++] = data & 0xFF;
              if( numSameData==outputLineLength )
                { 
                  // whole line was skipped
                  numSkippedLines++;
                  numSameData = 0; 
              
                  // we can skip further multiples of outputLineLength of the current data
                  numSkippedLines += count/outputLineLength;
                  count = count % outputLineLength;
                }
            }
          else
            {
              if( numData==0 ) printHexLineHeader(data);
              if( numData==8 && outputLineLength==16 ) serialBufferedWrite(' ');
              printHexLineData(data);
              lineBuf[numData++] = data & 0xFF;
              numSameData = (data==prevData) ? (numSameData+1) : 0;
              prevData = data;
            }

          if( numData==outputLineLength )
            {
              // printed a full line of data => finish it
              if( outputPrintASCII ) printASCIILineData(numData, lineBuf);
              serialBufferedWrite(13); serialBufferedWrite(10);
              
              // this line was all the same data => if enabled, enter line-skip mode (skip following lines containing same data)
              numSkippedLines = (numSameData>=(numData-1) && outputSuppressRepeats) ? 0 : -1;
              numData = 0;
              numSameData = 0;
            }
        }
    }
}


// -------------------------------------------------------------------------------------------------
// main functions
// -------------------------------------------------------------------------------------------------

RPI_PICO_Timer timer(1);

void setup() 
{
  busqueue_start = 0;
  busqueue_end = 0;
  busqueue_lock = spin_lock_init(spin_lock_claim_unused(true));

  outputPrintASCII = true;
  outputSuppressRepeats = true;
  outputLineLength = 16;
  memset(ports, 0xFF, 512);

  Serial.begin();

  EEPROM.begin(configSize()+3);
  loadConfig();

  for(int i=0; i<16; i++) pinMode(i, INPUT);
  pinMode(PIN_INPUT,    INPUT);
  pinMode(PIN_OUTPUT,   INPUT);
#ifdef DEBUG
  pinMode(PIN_CLR,      OUTPUT);
#else
  pinMode(PIN_CLR,      INPUT);
#endif

  // built-in LED
  pinMode(25, OUTPUT);

  // start bus data capture on second core
  multicore_launch_core1(core2);

#ifdef HAVE_DISPLAY
  pinMode(PIN_SR_LATCH, OUTPUT);
  pinMode(PIN_SR_CLK,   OUTPUT);
  pinMode(PIN_SR_DATA,  OUTPUT);
  pinMode(PIN_ROT_A,    INPUT_PULLUP);
  pinMode(PIN_ROT_B,    INPUT_PULLUP);
  pinMode(PIN_ROT_BTN,  INPUT_PULLUP);

  // enable 7-segment displays
  pinMode(PIN_SR_ENAB,  OUTPUT);
  digitalWrite(PIN_SR_ENAB, LOW);

  // start interrupts to update 7-segment LED displays
  timer.attachInterruptInterval(1000, updateLEDs);
#endif
}


void loop() 
{
  // only compute millis() once per cycle
  cycleTime = millis();

  // handle output of captured data via serial port
  handleSerialOutput();

#ifdef HAVE_DISPLAY
  // handle 7-segment display configuration
  handleLEDConfig();
#endif

#ifndef DEBUG
  static bool clrPrev = false;
  bool clr = (gpio_get_all() & bit(PIN_CLR))==0;
  if( clr != clrPrev )
    {
      delay(5);
      if( clr == ((gpio_get_all() & bit(PIN_CLR))==0) )
        {
          if( clr ) memset(ports, 0xFF, 512);
          clrPrev = clr;
        }
    }
#endif
}
