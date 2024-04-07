// pin 0-7:  data bus
// pin 8-15: address bus
#define PIN_INP    16
#define PIN_OUT    17

// status register bits
#define ST_RDRF 0x01  // 0=receiver data register full
#define ST_TDRE 0x80  // 0=transmitter data register empty

// registers
spin_lock_t* lock = NULL;
volatile uint8_t status, dataIn, dataOut;


// must protect writes to "status" via a spinlock since both
// cores may be modifying status bits at the same time
inline void set_status(uint8_t mask)
{
  uint32_t interrupts = spin_lock_blocking(lock);
  status = status | mask;
  spin_unlock(lock, interrupts);
}

inline void clr_status(uint8_t mask)
{
  uint32_t interrupts = spin_lock_blocking(lock);
  status = status & ~mask;
  spin_unlock(lock, interrupts);
}


// -------------------------------------------------------------------------------------------------
// bus communication process (running on second core)
// -------------------------------------------------------------------------------------------------


#define MASK_INOUT (bit(PIN_INP) | bit(PIN_OUT))

void __no_inline_not_in_flash_func(core2)()
{
  noInterrupts();
  uint32_t prevData = gpio_get_all();
  while(1) 
    {
      uint32_t data = gpio_get_all();
      if( (data & MASK_INOUT) != prevData )
        {
          // a change was seen on PIN_INP or PIN_OUT
          // this may have been just noise, induced by some other signal changing
          // => wait a short time and read the inputs again
          asm volatile ("nop\n nop\n");
          uint32_t data2 = gpio_get_all();
          if( (data2 & MASK_INOUT) == (data & MASK_INOUT) )
            data = data2;
          else
            continue;
          
          if( !(prevData & bit(PIN_INP)) && (data & bit(PIN_INP)) )
            {
              // IN operation on bus
              uint8_t port = (data >> 8) & 0xFF;
              if( port<2 )
                {
                  // either port 0 (status) or 1 (data)
                  gpio_set_dir_out_masked(0xFF);
                  gpio_put_masked(0xFF, port==0 ? status : dataIn);
                  if( port==1 ) set_status(ST_RDRF); // receive register is NOT full now
                  while( gpio_get_all() & bit(PIN_INP) );
                  gpio_set_dir_in_masked(0xFF);
                }
            }
          else if( !(prevData & bit(PIN_OUT)) && (data & bit(PIN_OUT)) )
            {
              // OUT operation on bus
              uint8_t port = (data >> 8) & 0xFF;
              if( port==1 ) 
                {
                  // port 1 (data)
                  dataOut = data & 0xFF;
                  set_status(ST_TDRE); // transmit register is NOT empty now
                }
            }

          prevData = (data & MASK_INOUT);
        }
    }
}


// -------------------------------------------------------------------------------------------------
// main functions
// -------------------------------------------------------------------------------------------------


void setup() 
{
  Serial.begin();

  for(int i=0; i<16; i++) pinMode(i, INPUT);
  pinMode(PIN_INP,  INPUT);
  pinMode(PIN_OUT, INPUT);
  
  // initially, receiver data register is NOT full
  // and transmitter data register is empty
  status = ST_RDRF;
  
  // initialize spin lock for "status" register access 
  lock = spin_lock_init(spin_lock_claim_unused(true));

  // start bus data capture on second core
  multicore_launch_core1(core2);
}


void loop() 
{
  if( Serial.available() && (status & ST_RDRF) )
    {
      // we have incoming data on serial and receiver data register is empty
      dataIn = Serial.read();
      clr_status(ST_RDRF); // receiver data register is full now
    }

  if( Serial.availableForWrite() && (status & ST_TDRE) )
    {
      // transmitter data register is full and we can send data to serial
      Serial.write(dataOut);
      clr_status(ST_TDRE); // transmitter data register is empty now
    }
}
