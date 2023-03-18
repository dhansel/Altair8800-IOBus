// NOTE: the read/write routines for WAV files assume the code to be running
// on a little-endian architecture (e.g. x86 or x64)

#include "Arduino.h"
#include <stdio.h>
#include <string.h>

struct WAV_HEADER
{
  char riff[4];                        // "RIFF" string
  unsigned int overall_size;           // overall size of file in bytes
  char wave[4];                        // "WAVE" string
  unsigned char fmt_chunk_marker[4];   // " fmt" string
  unsigned int length_of_fmt;          // length of the format data
  unsigned short format_type;          // format type. 1-PCM, 3- IEEE float, 6 - 8bit A law, 7 - 8bit mu law
  unsigned short channels;             // no of channels
  unsigned int sample_rate;            // sampling rate (blocks per second)
  unsigned int byterate;               // SampleRate * NumChannels * BitsPerSample/8
  unsigned short block_align;          // NumChannels * BitsPerSample/8
  unsigned short bits_per_sample;      // bits per sample, 8- 8bits, 16- 16 bits etc
  char data_chunk_header[4];           // "data" string
  unsigned int data_size;              // NumSamples * NumChannels * BitsPerSample/8
};

double sample_time_offset = 0.0;
bool keep_going = false, ignore_leader_length = false, quiet = false;
FILE *outfile = NULL, *cmpfile = NULL;
extern byte min_good_pulses;

void digitalWrite(int pin, bool v)
{ 
  if(pin==12 && !quiet) printf(v ? "LED-ON : %.6f\n" : "LED-OFF : %.6f\n", getTime()); 
}


byte readBus(byte reg)
{
  PINC = (PINC & 0xFE) | (reg & 1);
  PINB |= 0x02;
  ISR_PCINT0_vect();
  return PORTD;
}


void writeBus(byte reg, byte data)
{
  PIND = data;
  PINC = (PINC & 0xFE) | 0x04 | (reg & 1);
  ISR_PCINT1_vect();
}


double getTime()
{
  return sample_time_offset + double(clock)/double(clockfreq);
}


bool getData()
{
  static int n = 0;

  byte status = readBus(0);
  if( (status&0x08)!=0 ) 
    { printf("\nFRAMING ERROR t=%.6f\n", getTime()); return false; }
  else if( (status&0x04)!=0 )
    { printf("\nPARITY ERROR t=%.6f\n", getTime()); return false; }

  if( (status&0x01)==0 )
    {
      byte data = readBus(1);
#if SIMDEBUG>0
      printf("received byte: %02X (%02X), t=%.6f\n", data, regOut[0], getTime());
#else        
      if( !quiet ) 
        { 
          if( (n&15)==0 ) printf("%04X:", n);
          printf(" %02X", data); 
          if( ((n+1)&15)==8 ) 
            printf(" ");
          else if( ((n+1)&15)==0 ) 
            printf("\n"); 
        }
#endif

      if( outfile!=NULL ) fwrite(&data, 1, 1, outfile);

      if( cmpfile!=NULL )
        { 
          byte d; 
          
          if( ignore_leader_length )
            {
              static int leader_byte = 0x100;
              if( leader_byte==0x100 )
                {
                  fread(&d, 1, 1, cmpfile);
                  leader_byte = d;
                }
              else if( data==leader_byte )
                d = leader_byte;
              else
                {
                  while( fread(&d, 1, 1, cmpfile)==1 && d==leader_byte );
                  ignore_leader_length = false;
                }
            }
          else if( fread(&d, 1, 1, cmpfile)==0 )
            { printf("\nCompare data file end-of-data!\n"); fclose(cmpfile); cmpfile=NULL; d = data; }
          
          if( d!=data )
            { 
              printf("\nCompare data file difference t=%.6f: expected %02X, found %02X!\n", getTime(), d, data);
              return false;
            }
        }
      
      n++;
    }

  return (status & 0x0C)==0;
}


bool readCSV(const char *fname)
{
  FILE *f = fopen(fname, "rt");
  if(f == NULL) 
    { printf("Unable to open CSV file: %s\n", fname); return false; }

  // CSV capture only has a small number of samples before data start
  min_good_pulses = 50;

  int n = 0, offset = 99999999, pt = 0;
  printf("Reading file: %s\n", fname);
  while( !feof(f) )
    {
      int t, v;
      if( fscanf(f, "%i,%i", &t, &v)==2 )
        {
          //printf("X:%i %i %i\n", t, v, t-pt);
          if( offset==99999999 ) 
            {
              offset = t;
              sample_time_offset = double(t)/10000000.0;
            }

          pt = t;
          t -= offset;

          while( clock < t/10 )
            advanceClock();

          if( !getData() && !keep_going )
            return false;
                  
          setTimer1Capture(v!=0);
        }
    }

  fclose(f);
  return true;
}


bool readWAV(const char *fname)
{
  // open file
  FILE *f = fopen(fname, "rb");
  if(f == NULL) 
    { printf("Unable to open WAV file: %s\n", fname); return false; }
  
  // read header
  struct WAV_HEADER header;

  if( fread(&header, 1, sizeof(header), f)!=sizeof(header)
      || memcmp(header.riff, "RIFF", 4)!=0 
      || memcmp(header.fmt_chunk_marker, "fmt ", 4)!=0 
      || memcmp(header.wave, "WAVE", 4)!=0 
      || memcmp(header.data_chunk_header, "data", 4)!=0 
      || header.format_type != 1 )
    { printf("Not a (supported) WAV file!\n"); return false; }

  long num_samples = (8 * header.data_size) / (header.channels * header.bits_per_sample);
  long sample_size = (header.channels * header.bits_per_sample) / 8;
  long bytes_per_channel = (sample_size / header.channels);

  // make sure that the bytes-per-sample is completely divisible by num.of channels
  if ((bytes_per_channel  * header.channels) != sample_size) 
    { printf("Error: %ld x %ud <> %ld\n", bytes_per_channel, header.channels, sample_size); return false; }

  printf("Number of channels  : %u\n", header.channels);
  printf("Sample rate         : %lu\n", header.sample_rate);
  printf("Number of samples   : %lu\n", num_samples);
  printf("Size of each sample : %ld bytes\n", sample_size);
  printf("Duration in seconds : %f\n", (float) header.overall_size / header.byterate);

  double clockTicksPerSample = (double) clockfreq / (double) header.sample_rate;
  unsigned char data_buffer[sample_size];
     
  for(int i=1; i<=num_samples; i++)
    if( fread(data_buffer, sizeof(data_buffer), 1, f)==1 )
      {
        int sample_data = 0;
        if (bytes_per_channel == 4) 
          sample_data = *((int *) data_buffer);
        else if (bytes_per_channel == 2) 
          sample_data = *((short *) data_buffer);
        else if (bytes_per_channel == 1) 
          sample_data = *data_buffer-128;

        // set timer input capture value
        setTimer1Capture(sample_data>0);

        // advance clock according to WAV sample rate
        while( clock < int(i*clockTicksPerSample+0.5) ) advanceClock();
               
        // check serial data output
        if( !getData() && !keep_going )
          return false;
      }
 
  fclose(f);
  return true;
}


bool writeWAV(const char *fname)
{
  FILE *f = fopen(fname, "rb");
  if(f == NULL) 
    { printf("Unable to open TAP file: %s\n", fname); return false; }

  struct WAV_HEADER header;
  char sample = 0;
  byte data = 0;
  unsigned int n = 0;

  // leaderlen is timed precisely ("+200") such that start of CUTS data
  // will correlate with the end of a short pulse (on real hardware this
  // is automatically achieved by the "while( TCNT2 );" loop in ACR.ino
  // but that would block indefinitely in simulation)
  unsigned int leaderlen  = (3*clockfreq) + 200;
  unsigned int samplerate = 48000;
  unsigned int nextsample = 0;
  unsigned int endtime    = leaderlen + 1000;

  // leave space for WAV header
  fseek(outfile, sizeof(header), SEEK_SET);
      
  // write data
  while( clock<endtime )
    {
      while( clock<nextsample ) 
        {
          if( (readBus(0) & 0x80)==0 && clock>leaderlen && !feof(f) && fread(&data, 1, 1, f)==1 )
            {
              //printf("writing %02X\n", data);
              writeBus(1, data);
              endtime = clock + clockfreq;
            }
              
          advanceClock();
        }

      static bool prevOutput = false;
      static unsigned int prevClock = 0;
      if( getTimer2OutputA()!=prevOutput )
        {
          if( prevClock>0 && clock-prevClock > 1250 )
            {
              printf("FAIL: %i %02X\n", clock-prevClock, TCCR2B);
              clock = endtime;
            }
          prevOutput = getTimer2OutputA();
          prevClock=clock;
        }
          
      //printf("sample: %.6f, %i, %i\n", getTime(), clock, getTimer2OutputA());
      //printf(getTimer2OutputA() ? "1" : "0");
      sample = (sample*2 + (getTimer2OutputA() ? 120 : -120))/3;
      data = sample+128;
      fwrite(&data, 1, 1, outfile);

      nextsample = double(++n) * (double(clockfreq)/double(samplerate)) + 0.5;
    }

  // write header
  fseek(outfile, 0, SEEK_SET);
  memcpy(header.riff, "RIFF", 4);
  header.overall_size = 44+n-8;
  memcpy(header.wave, "WAVE", 4);
  memcpy(header.fmt_chunk_marker, "fmt ", 4);
  header.length_of_fmt = 16;
  header.format_type = 1;
  header.channels = 1;
  header.sample_rate = samplerate;
  header.byterate = samplerate;
  header.block_align = 1;
  header.bits_per_sample = 8;
  memcpy(header.data_chunk_header, "data", 4);
  header.data_size = n;
  fwrite(&header, 1, sizeof(header), outfile);

  fclose(f);
  return true;
}


int main(int argc, char **argv)
{
  int i;
  bool legacy_mode = false, speed_skew_comp = false;
  const char *format = "MITS";

  if( argc<2 )
    {
      printf("Usage: ACR [-qlks] [-f MITS|CUTS|KCS] [-cC comparedatafile] [-o outfile] infile[.wav|.csv|.tap]\n");
      return 0;
    }

  for(i=1; i<argc; i++)
    {
      if( strcmp(argv[i], "-f")==0 && i+1<argc )
        format = argv[++i];
      else if( strcmp(argv[i], "-o")==0 && i+1<argc )
        {
          outfile = fopen(argv[++i], "wb+");
          if( outfile==NULL ) 
            { printf("Unable to open output file for writing: %s\n", argv[i]); return 0; }
          else
            { printf("Writing output data to file: %s\n", argv[i]); }
        }
      else if( stricmp(argv[i], "-c")==0 && i+1<argc )
        {
          ignore_leader_length = argv[i][1]=='C';
          cmpfile = fopen(argv[++i], "rb");
          if( cmpfile==NULL )
            { printf("Unable to open compare data file: %s\n", argv[i]); return 0; }
          else
            { printf("Comparing data with contents of file: %s\n", argv[i]); }
        }
      else if( strcmp(argv[i], "-l")==0 )
        legacy_mode = true;
      else if( strcmp(argv[i], "-k")==0 )
        keep_going = true;
      else if( strcmp(argv[i], "-q")==0 )
        quiet = true;
      else if( strcmp(argv[i], "-s")==0 )
        speed_skew_comp = true;
      else if( argv[i][0]=='-' )
        printf("Unknown option: %s\n", argv[i]);
      else
        break;
    }

  // set hardware-configurable options
  if( speed_skew_comp ) PINC &= ~0x20; else PINC |= 0x20;
  if( legacy_mode ) PINB &= ~0x04; else PINB |= 0x04;

  setup();

  const char *fname = NULL;
  if( i<argc )
    fname = argv[i];
  else
    { printf("No input file name.\n"); return 1; }
          
  if( stricmp(format, "MITS")==0 )
    writeBus(0, 0x00);
  else if( stricmp(format, "KCS")==0 )
    writeBus(0, 0xA0);
  else if( stricmp(format, "CUTS")==0 )
    writeBus(0, 0x80);
  else
    { printf("unknown tape format specifier: %s\n", format); return 1; }

  printf("Using '%s' tape format.\n", legacy_mode ? "MITS" : format);

  bool ok = true;
  char *c = strrchr(fname, '.');
  if( c==NULL || stricmp(c+1, "wav")==0 )
    ok = readWAV(fname);
  else if( stricmp(c+1, "csv")==0 )
    ok = readCSV(fname);
  else
    {
      if( outfile==NULL )
        { printf("Output file required when generating WAV data.\n"); return 1; }
      else if( cmpfile!=NULL )
        { printf("Cannot compare output when generating WAV file.\n"); return 1; }
      else
        ok = writeWAV(fname);
    }
  
 if( cmpfile!=NULL && ok )
   {
     printf("\nNo differences found!\n");
     unsigned int n1 = ftell(cmpfile);
     fseek(cmpfile, 0, SEEK_END);
     unsigned int n2 = ftell(cmpfile);
     if( n1==0 )
       { printf("Did not read any data!\n"); return 1; }
     else if( n2>n1 ) 
       printf("Compare data file has %i bytes of data left.\n", n2-n1);
   }
 
  if( cmpfile!=NULL ) fclose(cmpfile);
  if( outfile!=NULL ) fclose(outfile);

  return ok ? 0 : 1;
}
