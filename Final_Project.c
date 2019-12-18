/*
 * File:        Final Project for ECE 4760
 *             
 * Author:      Zihao Xue
 * For use with Sean Carroll's Big Board
 * http://people.ece.cornell.edu/land/courses/ece4760/PIC32/target_board.html
 * Target PIC:  PIC32MX250F128B
 */

#include "config_1_3_2.h"
// threading library
#include "pt_cornell_1_3_2.h"
#include <stdint.h>
#include <math.h>
////////////////////////////////////
// graphics libraries
// SPI channel 1 connections to TFT
#include "tft_master.h"
#include "tft_gfx.h"
/* Demo code for interfacing TFT (ILI9340 controller) to PIC32
 * The library has been modified from a similar Adafruit library
 */
// Adafruit data:
/***************************************************
  This is an example sketch for the Adafruit 2.2" SPI display.
  This library works with the Adafruit 2.2" TFT Breakout w/SD card
  ----> http://www.adafruit.com/products/1480
  Check out the links above for our tutorials and wiring diagrams
  These displays use SPI to communicate, 4 or 5 pins are required to
  interface (RST is optional)
  Adafruit invests time and resources providing this open source code,
  please support Adafruit and open-source hardware by purchasing
  products from Adafruit!
  Written by Limor Fried/Ladyada for Adafruit Industries.
  MIT license, all text above must be included in any redistribution
 ****************************************************/
#include <stdlib.h>
#include <stdio.h>
// SPI channel 1 also connections to SD adapter
#include "FSIO.h"
#include <GenericTypeDefs.h>
#include <stdbool.h>
////////////////////////////////////
#define pwm_on_time 0
#define SYS_CLK         40000000
#define SPI_FRE         20000000
////////////////////////////////////
uint8_t sampledepth;
uint8_t channels;
UINT32 dataSize;
uint32_t sampleRate;
uint16_t blockAlign;
// file pointer
FSFILE * fp;
// DAC ISR
// A-channel, 1x, active
#define DAC_config_chan_A 0b0011000000000000
// B-channel, 1x, active
#define DAC_config_chan_B 0b1011000000000000
#define ZERO    2048    // 0 - 2047
#define BufferSize  2000
volatile uint16_t LeftAudioData[BufferSize];
volatile uint16_t RightAudioData[BufferSize];
#define  DrawLength   1000
volatile int16_t  DrawArray[DrawLength]; //Store the rescaled audio data to draw the amplitude on the TFT
volatile uint8_t  DrawIndex = 0;
volatile uint8_t  TempIndex;             // Tell which rectangular in DrawArray to draw on the TFT
volatile bool Playenabled = false;       // Flag to determine whether to send data to the DAC
volatile bool startread = false;		 // Flag to determine whether to execute the reader thread
volatile uint16_t WriteIndex = 0;		 // Write index to indicate the current index of data that should be sent to the DAC
volatile uint16_t ReadIndex = 0;		 // Read index to indicate the current index of data that should be store in the cyclic buffers
volatile UINT32 TotalWriteNum = 0;		 // Record how many bytes already write
volatile UINT32 TotalReadNum = 0;		 // Record how many bytes already read 	
volatile size_t LeftPWM = 0;             // PWM output to control the speed of left wheel
volatile size_t RightPWM = 0;            // PWM output to control the speed of right wheel
volatile bool playmode = false;          // Whether in piano mode

void __ISR(_TIMER_2_VECTOR, ipl2) Timer2Handler(void) {
	mT2ClearIntFlag();
	// output PWM signals
	SetDCOC1PWM(LeftPWM);
	SetDCOC2PWM(RightPWM);
}
volatile uint8_t audioStream[BufferSize * 4];
inline void writeDAC(uint16_t dacVal) {
	int junk;
	while (TxBufFullSPI2());
	while (SPI2STATbits.SPIBUSY);
	// wait for end of the last tranmission
	// CS low to start transaction
	// printf for debugging purpose
	//printf("%u %u\r\n", TotalWriteNum, dataSize);
	mPORTBClearBits(BIT_4);
	// start transaction
	// write to spi2 
	WriteSPI2( dacVal );
	while (SPI2STATbits.SPIBUSY);
	// wait for end of transaction
	// CS high
	mPORTBSetBits(BIT_4);
	// end transaction
	// need to read SPI channel to avoid confusing TFT
	junk = ReadSPI2();
}


volatile bool end = false;
volatile bool MusicMode = true;

void __ISR(_TIMER_3_VECTOR, ipl2) Timer3Handler(void) {
	if (Playenabled) {
		// DDS_phase += DDS_increment ;
		//DAC_data += 1 & 0xfff ; // low frequency ramp
		// DAC_data_A = (int)(max_amplitude*sine_table[DDS_phase>>24]) + 2048 ; // for testing sine_table[DDS_phase>>24]
		writeDAC( LeftAudioData[WriteIndex] );
		writeDAC( RightAudioData[WriteIndex] );
		if (++WriteIndex >= BufferSize) {
			WriteIndex = 0;
		}
		TotalWriteNum += blockAlign;
		// printf("%u %u\r\n", TotalWriteNum, dataSize);
		// If it is in speaker mode 
        if(MusicMode)
        {   // If it reaches the end of the music, then stop reading data from the SD card
	        // and stop sending data the DAC.
            if (TotalWriteNum >= TotalReadNum) {
                Playenabled = false;
                startread = false;
                end = true;
            }
        }
        else
        {
			// If it almost reaches the end of the piano note, then stop reading data from the SD card
	        // and stop sending data the DAC.
            if (TotalWriteNum >= TotalReadNum * 59/60) {
                Playenabled = false;
                startread = false;
                end = true;
            }

        }

	}
	mT3ClearIntFlag();
}
// === thread structures ============================================
// thread control structs
// note that UART input and output are threads
static struct pt pt_timer, pt_serial, pt_reader, pt_anim;
// The following threads are necessary for UART control
static struct pt pt_input ;
// system 1 second interval tick
int sys_time_seconds ;
// === Timer Thread =================================================
// update a 1 second tick counter
static PT_THREAD (protothread_timer(struct pt *pt))  {
	PT_BEGIN(pt);
	// set up LED to blink for testing mode
	mPORTASetPinsDigitalOut(BIT_0 );
	//Set port as output
	mPORTASetBits(BIT_0 );
	//Clear bits to ensure light is off.
	while(1) {
		// yield time 1 second
		PT_YIELD_TIME_msec(1000) ;
		mPORTAToggleBits(BIT_0 );
		// NEVER exit while
	}
	// END WHILE(1)
	PT_END(pt);
}
// timer thread

static PT_THREAD (protothread_anim(struct pt *pt)) {
	PT_BEGIN(pt);
	while(1) {
		// yield time 1 second
		PT_YIELD_TIME_msec(100) ;
		// If it is in the speaker mode, when reaching the end of playing the music,
		// ReWrite the TFT display. And only rewrite the TFT once.
		if (MusicMode) {
			if (end) {
				tft_fillScreen(ILI9340_BLACK);
				TFT_ECE();
				end = false;
			}
		}
	}
	// END WHILE(1)
	PT_END(pt);
}
// anim thread

//  Get the audio specifications based on the first 44 bytes of the .wav files
void GetMusicInfo (void) {
	char header[50];
	// Read the 44 bytes into the header buffer to calculate the audio specifications
	FSfread(header, 1, 44, fp);
	sampleRate = (header[24] | ((uint32_t)header[25] << 8))&(0xffff);
	sampledepth = header[34];
	channels = header[22];
	dataSize = header[40]| (header[41] << 8) | (header[42] << 16) | (header[43] << 24);
	blockAlign = header[32];
}


// While in piano mode, first drawing the keyBoard on the TFT display
inline void DrawKeyBoard(void) {
	tft_fillScreen(ILI9340_YELLOW);
	tft_fillRect(35,5, 130, 30, ILI9340_WHITE);
	// x,y,w,h,radius,color
	tft_fillRect(35,40, 130, 30, ILI9340_WHITE);
	// x,y,w,h,radius,color
	tft_fillRect(35,75, 130, 30, ILI9340_WHITE);
	// x,y,w,h,radius,color
	tft_fillRect(35,110, 130, 30, ILI9340_WHITE);
	// x,y,w,h,radius,color
	tft_fillRect(35,145, 130, 30, ILI9340_WHITE);
	// x,y,w,h,radius,color
	tft_fillRect(35,180, 130, 30, ILI9340_WHITE);
	// x,y,w,h,radius,color
	tft_fillRect(35,215, 130, 30, ILI9340_WHITE);
	// x,y,w,h,radius,color
	tft_fillRect(35,250, 130, 30, ILI9340_WHITE);
	// x,y,w,h,radius,color
	tft_fillRect(35,285, 130, 30, ILI9340_WHITE);
	// x,y,w,h,radius,color
	tft_fillRect(135,23, 100, 30, ILI9340_BLACK);
	// x,y,w,h,radius,color
	tft_fillRect(135,58, 100, 30, ILI9340_BLACK);
	// x,y,w,h,radius,color
	tft_fillRect(135,93, 100, 30, ILI9340_BLACK);
	// x,y,w,h,radius,color
	tft_fillRect(135,128, 100, 30, ILI9340_BLACK);
	// x,y,w,h,radius,color
	tft_fillRect(135,163, 100, 30, ILI9340_BLACK);
	// x,y,w,h,radius,color
	tft_fillRect(135,198, 100, 30, ILI9340_BLACK);
	// x,y,w,h,radius,color
	tft_fillRect(135,233, 100, 30, ILI9340_BLACK);
	// x,y,w,h,radius,color
	tft_fillRect(135,268, 100, 30, ILI9340_BLACK);
	// x,y,w,h,radius,color
}
char buffer[60];
//Debug purpose!

volatile uint8_t pianoindex = -1;    // To record which key is pressed 
volatile uint8_t lastpianoindex = -1;// To record the last key that is pressed
volatile bool rewrite;               // To indicate whether there is a need to rewrite the TFT display
const uint8_t HashTable[20] = {0,0,0,2,0,3,4,5,0,6,7,8,0,0,0,0,0,0,1,0};   // The look-up table to calculate the current key index
#define RESCALE  'a'


static PT_THREAD (protothread_serial(struct pt *pt)) {
	PT_BEGIN(pt);
	while(1) {
		static char cmd[10];
		//PT_GetMachineBuffer 
		PT_SPAWN(pt, &pt_input, PT_GetMachineBuffer(&pt_input) );
		if(PT_timeout==0) {
			sscanf(PT_term_buffer, "%s", cmd);
			/* tft_fillRoundRect(0,100, 230, 15, 1, ILI9340_BLACK);// x,y,w,h,radius,color
                     tft_setTextColor(ILI9340_YELLOW); tft_setTextSize(2);     
                     tft_setCursor(0, 100);
			//sprintf(buffer, "%d", error );
			tft_writeString(cmd);*/
		} else {
			cmd[0] = 0 ;
			LeftPWM = 0;
			RightPWM = 0;
		}
		if(MusicMode) {
			switch(cmd[0]) {
				// Convert to default mode: Speaker mode and Driving mode
				case 'm':
				MusicMode = true;
                PT_terminate_time = 50;
				playmode = false;
				break;
				
				// Convert to piano mode
				case 'n':
                PT_terminate_time = 250;
                MusicMode = false;
				Playenabled = false;
				startread = false;
				playmode = false;
				rewrite = true;
				break;
				case 'w':
				                    // Moving forward
				mPORTASetBits(BIT_3); 
				mPORTBSetBits(BIT_9);
				mPORTAClearBits(BIT_4);
				mPORTAClearBits(BIT_2);
				LeftPWM = 15000;
				RightPWM = 15000;
				break;
				case 's':
				                    // Moving backward
				mPORTASetBits(BIT_4);
				mPORTASetBits(BIT_2);
				mPORTAClearBits(BIT_3);
				mPORTBClearBits(BIT_9);
				LeftPWM = 15000;
				RightPWM = 15000;
				break;
				case 'a': 
				                     // Turning left
				mPORTASetBits(BIT_3);
				mPORTBSetBits(BIT_9);
				mPORTAClearBits(BIT_4);
				mPORTAClearBits(BIT_2);
				LeftPWM = 0;
				RightPWM = 25000;
				break;
				case 'd':
				                    // Turning right
				mPORTASetBits(BIT_3);
				mPORTBSetBits(BIT_9);
				mPORTAClearBits(BIT_4);
				mPORTAClearBits(BIT_2);
				LeftPWM = 25000;
				RightPWM = 0;
				break;
				case 'c': {
					// If it is not in the speaker mode and convert into speaker mode at the first time, then rewrite TFT display
					if(!playmode) {
						tft_fillScreen(ILI9340_BLACK);
						playmode = true;
					}
					// Open the specific audio file
					fp = FSfopen(&cmd[1],"r");
					if (fp != NULL) {
						// CloseTimer3();
						GetMusicInfo();
						// Based on the sample rate, configure the timer period
						OpenTimer3(T3_ON|T3_SOURCE_INT|T3_PS_1_1, SYS_CLK/sampleRate-1);
						ConfigIntTimer3(T3_INT_ON|T3_INT_PRIOR_2);
						mT3ClearIntFlag();
						// Initializatize all the variables
						ReadIndex = 0;
						WriteIndex = 0;
						TotalWriteNum = 0;
						TotalReadNum = 0;
						end = false;
						Playenabled = false;
						startread = true;
					}
					break;
				}
				// Pause or play the music
				case 'p': {
					Playenabled = !Playenabled;
					startread = !startread;
					break;
				}
				// 'q' means quit. It closes the audio file and stops reading audio data from the SD card as well
				// as stops sending audio data to the DAC
				case 'q': {
					Playenabled = false;
					startread = false;
					if (playmode) {
						FSfclose(fp);
					}
					break;
				}
				default:
				                     break;
			}
		} else {
			// If it is not in the piano mode and convert into piano mode at the first time, then rewrite TFT display with keypads
			if (rewrite) {
				DrawKeyBoard();
				rewrite = false;
			}
			switch(cmd[0]) {
				// It is in the piano mode, then converts to the default mode 
				case 'm':
                    PT_terminate_time = 50;
                    playmode = false;
                    MusicMode = true;
                    break;
				// In piano mode, press certain key to play certain note	
				case 'b': {
					if (playmode) {
						FSfclose(fp);
					}
                    pianoindex = HashTable[cmd[1] - RESCALE];
					playmode = true;
					fp = FSfopen(&cmd[1],"r");
					if (fp != NULL) {
						// CloseTimer3();
						GetMusicInfo();
						OpenTimer3(T3_ON|T3_SOURCE_INT|T3_PS_1_1, SYS_CLK/sampleRate-1);
						ConfigIntTimer3(T3_INT_ON|T3_INT_PRIOR_2);
						mT3ClearIntFlag();
						ReadIndex = 0;
						WriteIndex = 0;
						TotalWriteNum = 0;
						TotalReadNum = 0;
						end = false;
						Playenabled = false;
						startread = true;
					}
					break;
				}
				default:
				    break;
			}
		}
	}
	// END WHILE(1)
	PT_END(pt);
}
// thread_serial
#define FirstRead 1800   // It is used to guarantee that the total number of the first reading process is larger than 0.1 * sampleRate
volatile uint16_t audioByte;  
volatile uint16_t ByteNum;
volatile int Indexdiff;  // The difference between Read Index and Write Index 
volatile uint16_t ReadCount;  // The total number of bytes that should be read from the SD card
volatile short  Height = 0;   // Rescaled audio amplitude
#define ScaleFactor  (_Accum)(1<<12)   // Rescale factor
#define TFTHeight    (_Accum)ILI9340_TFTHEIGHT  // Rescale factor for piano mode
volatile uint16_t LineColor;

static PT_THREAD (protothread_reader(struct pt *pt)) {
	PT_BEGIN(pt);
	while(1) {
		static uint16_t i;
		PT_YIELD_TIME_msec(10) ;
		if (startread) {
			if (!Playenabled) {
				// if (!Playenabled) means it is the first time to execute this thread and in order to guarantee the audio signal is continuous
				// Here firstly reads a bunch of data into the buffer and then enables to execute the process of sending audio data to the DAC
				ByteNum = FSfread(audioStream, 1, FirstRead * blockAlign, fp);
				for (i = 0; i < ByteNum; i += blockAlign) {
					if (sampledepth == 16) {
						// If the audio data is 16-bit, then the 12-bit data is obtained by combining the 8 bits of the 
						// higher byte with the 4 bits of the lower byte.
						audioByte = (audioStream[i+1] << 4) |(audioStream[i] >> 4);
						DrawArray[DrawIndex++] = audioByte;
						if (DrawIndex >= DrawLength) {
							DrawIndex = 0;
						}
						// add channel A specification to the audio data
						LeftAudioData[ReadIndex] = DAC_config_chan_A | ((audioByte + ZERO) & 0xfff);
						if (channels == 2) {
							audioByte = (audioStream[i+3] << 4) | (audioStream[i+2] >> 4);
						}
						// add channel B specification to the audio data
						RightAudioData[ReadIndex] = DAC_config_chan_B | ((audioByte + ZERO) & 0xfff);
						ReadIndex++;
					} else {
						// As for 8-bit samples, the 12-bit data is obtained by bit-shifting left 4 times.
						audioByte = audioStream[i] << 4;
						DrawArray[DrawIndex++] = audioByte;
						if (DrawIndex >= DrawLength) {
							DrawIndex = 0;
						}
						// add channel A specification to the audio data
						LeftAudioData[ReadIndex] = DAC_config_chan_A | audioByte;
						if (channels == 2) {
							audioByte = audioStream[i+1] << 4;
						}
						// add channel B specification to the audio data
						RightAudioData[ReadIndex] = DAC_config_chan_B | audioByte;
						ReadIndex++;
					}
				}
				// Increase the total number of bytes read from the SD card
				TotalReadNum += ByteNum;
				Playenabled = true;  // Since there is some audio data in the cyclic buffers, it can start the process of sending data 
				// to the DAC
			} else {
				if (TotalReadNum < dataSize) {
					if ((TotalReadNum/blockAlign - TotalWriteNum/blockAlign) < BufferSize ) // If there is some space avaiable to store the
					// incoming audio data
					{
						// calculate the difference between the read index and write index
						Indexdiff = ReadIndex - WriteIndex;
						ReadCount = Indexdiff > 0 ? BufferSize - Indexdiff: -Indexdiff;
						if ( TotalReadNum + ReadCount * blockAlign >= dataSize) {
							startread = false;
							ReadCount = (dataSize - TotalReadNum) / blockAlign;
						}
						// Read some audio data into the audioStream buffer
						ByteNum = FSfread(audioStream, 1, ReadCount * blockAlign, fp);
						for (i = 0; i < ByteNum; i += blockAlign) {
							if (sampledepth == 16) {
								// If the audio data is 16-bit, then the 12-bit data is obtained by combining the 8 bits of the 
								// higher byte with the 4 bits of the lower byte.
								audioByte = (audioStream[i+1] << 4) |(audioStream[i] >> 4);
								DrawArray[DrawIndex++] = audioByte;
								if (DrawIndex >= DrawLength) {
									DrawIndex = 0;
								}
								LeftAudioData[ReadIndex] = DAC_config_chan_A | ((audioByte + ZERO) & 0xfff);
								if (channels == 2) {
									audioByte = (audioStream[i+3] << 4) | (audioStream[i+2] >> 4);
									RightAudioData[ReadIndex] = DAC_config_chan_B | ((audioByte + ZERO) & 0xfff);
								}
								if ( ++ReadIndex == BufferSize ) {
									ReadIndex = 0;
								}
							} else {
								// As for 8-bit samples, the 12-bit data is obtained by bit-shifting left 4 times.
								audioByte = audioStream[i] << 4;
								if (DrawIndex >= DrawLength) {
									DrawIndex = 0;
								}
								LeftAudioData[ReadIndex] =  DAC_config_chan_A | (audioByte + ZERO);
								if (channels == 2) {
									audioByte = audioStream[i+1] << 4;
								}
								RightAudioData[ReadIndex] = DAC_config_chan_B | (audioByte + ZERO);
								if ( ++ReadIndex == BufferSize ) {
									ReadIndex = 0;
								}
							}
						}
						// Increase the total number of bytes read from the SD card
						TotalReadNum += ByteNum;
					}
					//  It's in the default mode, and draws the rescaled amplitude of the audio signal
					if (MusicMode) {
						TempIndex = DrawIndex - 1 < 0 ?  DrawLength - 1 : DrawIndex-1;
						tft_fillRect(80,Height, 80, 5, ILI9340_BLACK);
						// x,y,w,h,radius,color
						Height = (short)((_Accum)(DrawArray[TempIndex])/ScaleFactor * TFTHeight);
						// printf("%d  %d %d \r\n", DrawArray[TempIndex], (int)ScaleFactor, Height);
						Height = Height < 0? -Height : Height;
						tft_fillRect(80,Height, 80, 5, ILI9340_YELLOW);
						// x,y,w,h,radius,color
					}
                    else
                    {
						
						//  It's in the piano mode, and draws the specific key that is pressed
                        if (lastpianoindex == -1)
                        {
                            tft_fillRect(80, 20 + pianoindex * 35, 40, 5, ILI9340_RED);
                        }
                        else if (lastpianoindex != pianoindex)
                        {
                            tft_fillRect(80, 20 + lastpianoindex * 35,40, 5, ILI9340_WHITE);
                            tft_fillRect(80, 20 + pianoindex * 35, 40, 5, ILI9340_RED);
                        }
                        lastpianoindex = pianoindex;
                    }
				}
			}
		}
		
		/*tft_fillRoundRect(0,100, 230, 15, 1, ILI9340_BLACK);// x,y,w,h,radius,color
                     tft_setTextColor(ILI9340_YELLOW); tft_setTextSize(2);     
                     tft_setCursor(0, 100);
                     sprintf(buffer, "%u", TotalWriteNum);
                     tft_writeString(buffer);
                     tft_setCursor(0, 200);
                     sprintf(buffer, "%u", dataSize);
                     tft_writeString(buffer);*/
		// NEVER exit while
	// END WHILE(1)
	PT_END(pt);
}
// reader thread



inline void TFT_wait() {
	// Pull high the chip select line before initializing the MDDFS
	TRIS_rst = 0;
	_rst_low();
	TRIS_dc = 0;
	TRIS_cs = 0;
	_dc_low();
	_cs_high();
}
inline void TFT_initialization() {
	tft_init_hw();
	tft_begin();
	tft_fillScreen(ILI9340_BLACK);
	//240x320 vertical display
	tft_setRotation(0);
	// Use tft_setRotation(1) for 320x240
	// Draw "ECE 4760" on the TFT display
	TFT_ECE();
}
inline void TFT_ECE() {
	tft_setTextColor(ILI9340_GREEN);
	tft_setTextSize(10);
	tft_setCursor(28, 100);
	tft_writeString("ECE");
	tft_setTextColor(ILI9340_YELLOW);
	tft_setTextSize(10);
	tft_setCursor(5, 200);
	tft_writeString("4760");
}
void HardwareInit() {
	// Set up timer2 on and open output compare unit 
	// L298N control pins
	mPORTBSetPinsDigitalOut(BIT_9);
	mPORTASetPinsDigitalOut(BIT_2|BIT_3|BIT_4);
	mPORTASetBits(BIT_4);
	mPORTASetBits(BIT_2);
	mPORTAClearBits(BIT_3);
	mPORTBClearBits(BIT_9);
	OpenTimer2(T2_ON|T2_SOURCE_INT|T2_PS_1_1, 40000);
	ConfigIntTimer2(T2_INT_ON|T2_INT_PRIOR_2);
	mT2ClearIntFlag();
	// PPS for output compare unit to output PWM signals
	PPSOutput(1,RPB7,OC1);
	PPSOutput(2,RPB8,OC2);
	OpenOC1(OC_ON|OC_TIMER2_SRC|OC_PWM_FAULT_PIN_DISABLE,pwm_on_time,pwm_on_time);
	OpenOC2(OC_ON|OC_TIMER2_SRC|OC_PWM_FAULT_PIN_DISABLE,pwm_on_time,pwm_on_time);
	// set up DAC on big board
	// control CS for DAC
	mPORTBSetPinsDigitalOut(BIT_4);
	mPORTBSetBits(BIT_4);
	PPSOutput(2, RPB5, SDO2);
	// 16 bit transfer CKP=1 CKE=1
	// Open SPI2 channel for the DAC 
	SpiChnOpen(SPI_CHANNEL2, SPI_OPEN_ON | SPI_OPEN_MODE16 | SPI_OPEN_MSTEN | SPI_OPEN_CKE_REV , 2);
	// UART is opened in PT_setup()
	/*// set up UART, which is connected to BlueTooth
    PPSInput (2, U2RX, RPA1); //Assign U2RX to pin RPA1 -- Physical pin 3 on 28 PDIPS
    PPSOutput(4, RPB10, U2TX); //Assign U2TX to pin RPB10 -- Physical pin 21 on 28 PDIP
    UARTConfigure(UART2, UART_ENABLE_PINS_TX_RX_ONLY);
    UARTSetLineControl(UART2, UART_DATA_SIZE_8_BITS | UART_PARITY_NONE | UART_STOP_BITS_1);
    UARTSetDataRate(UART2, SYS_CLK, BAUDRATE);
    UARTEnable(UART2, UART_ENABLE_FLAGS(UART_PERIPHERAL | UART_RX | UART_TX));*/
	// TFT_wait would guarantee to deselect the TFT and initialize File System first! Very important!
	TFT_wait();
	// Always return 1 since there is no SD detect line here
	while (!MDD_MediaDetect());
	// initialize MDDFS
	while (!FSInit());
	// After finishing the initialization of file system, reconfigure the frequency of SPI1
	SpiChnSetBitRate(SPI_CHANNEL1, SYS_CLK, SPI_FRE);
	// Initialize TFT
	TFT_initialization();
}
/*uint16_t colors[7] = {ILI9340_BLUE, ILI9340_RED, ILI9340_GREEN, ILI9340_CYAN, ILI9340_MAGENTA, 
ILI9340_YELLOW, ILI9340_WHITE};*/
// === Main  ======================================================
void main(void) {
	// Disable analog inputs
	ANSELA = 0;
	ANSELB = 0;
	PT_setup();
	// Initializations
	HardwareInit();
	//DrawingObjectInit();
	// === setup system wide interrupts  ========
	INTEnableSystemMultiVectoredInt();

	// init the threads
	PT_INIT(&pt_timer);
	PT_INIT(&pt_serial);
	PT_INIT(&pt_reader);
	PT_INIT(&pt_anim);
	// round-robin scheduler for threads

	while (1) {
		PT_SCHEDULE(protothread_timer(&pt_timer));
		PT_SCHEDULE(protothread_serial(&pt_serial));
		PT_SCHEDULE(protothread_reader(&pt_reader));
		PT_SCHEDULE(protothread_anim(&pt_anim));
	}
}
// main