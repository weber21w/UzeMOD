#include <avr/io.h>
#include <stdlib.h>
#include <string.h>
#include <avr/pgmspace.h>
#include <uzebox.h>
#include <spiram.h>
#include <petitfatfs/pffconf.h>
#include <petitfatfs/diskio.h>
#include <petitfatfs/pff.h>
#include "data/tiles.inc"
//#include "uzenet/unCore.h"

extern u8 ram_tiles[];
extern u8 vram[];
extern u8 mix_buf[];
extern BYTE rcv_spi();
#define	SD_SELECT()	PORTD &=  ~(1<<6)
#define	SD_DESELECT()	PORTD |=  (1<<6)

extern volatile u8 mix_bank;
extern u16 joypad1_status_lo, joypad2_status_lo;
extern u16 joypad1_status_hi, joypad2_status_hi;
#define Wait200ns() asm volatile("lpm\n\tlpm\n\t")

#define UZENET_EEPROM_ID0	32
#define UZENET_EEPROM_ID1	UZENET_EEPROM_ID0+1

#define NO_FINE_TUNE_TABLE	0
#define NO_SAMPLE_VOLUME	0
#define DEFAULT_SAMPLE_VOLUME	64

#define MOD_BASE		0UL//1024*3
#define SAMPLE_HEADER_BASE	(MOD_BASE + 20UL)
#define SAMPLE_HEADER_SIZE	(22UL + 2UL + 1UL + 1UL + 2UL + 2UL) // 30 bytes
#define PATTERN_BASE		(MOD_BASE + 1084UL)
#define ORDER_TAB_OFF		(MOD_BASE + 952UL)

#define MOD_CHANNELS		4
#define MAX_SAMPLES		31
#define SAMPLES_PER_FRAME	262
#define SAMPLE_RATE		(SAMPLES_PER_FRAME * 60UL)
#define PAULA_RATE		((3546895UL / SAMPLE_RATE) << 16UL)
#define DEFAULT_AUDIO_SPEED	(SAMPLE_RATE/50UL)

#define BUFFER_SIZE		 262

#define CONT_BTN_W	16
#define CONT_BAR_X	40
#define CONT_BAR_W	13*CONT_BTN_W
#define CONT_BTN_H	CONT_BTN_W
#define CONT_BAR_Y	0
#define CONT_BAR_H	CONT_BTN_H

#define TILE_CURSOR	129
#define TILE_WIN_TLC	TILE_CURSOR+1
#define TILE_WIN_TRC	TILE_WIN_TLC+1
#define TILE_WIN_BLC	TILE_WIN_TRC+3
#define TILE_WIN_BRC	TILE_WIN_BLC+1
#define TILE_WIN_TBAR	TILE_WIN_TRC+1
#define TILE_WIN_BBAR	TILE_WIN_TBAR+1
#define TILE_WIN_LBAR	TILE_WIN_BRC+1
#define TILE_WIN_RBAR	TILE_WIN_LBAR+1
#define TILE_WIN_SCRU	TILE_WIN_RBAR+1
#define TILE_WIN_SCRD	TILE_WIN_SCRU+1

typedef struct{
	u32 spiBase;	//offset in SPI RAM
	u32 currentptr;	//integer playback pointer
	u32 length;	//total sample length in bytes
	u32 loopStart;	//loop start offset in bytes
	u32 looplength;	//loop length in bytes
	u32 period;	//fractional step accumulator
	u8 volume;//s32 volume;	//current volume 0..64
	u32 currentsubptr; //fractional sub-pointer (lower 16 bits used)
}PaulaChannel_t;

typedef struct{
	u32 length;	//full sample length in bytes
	u32 loopStart;	//loop start offset in bytes
	u32 looplength;	//loop length in bytes
	u8 finetune;
	u8 volume;
	u32 spiBase;
}Sample_t;

typedef struct{
	s16 val;
	u8 waveform;
	u8 phase;
	u8 speed;
	u8 depth;
}Oscillator_t;

typedef struct{
	u32 note;
	u8 sample, eff, effval;
	u8 slideamount, sampleoffset;
	s16 volume;
	s32 slidenote;
	u32 period;
	Oscillator_t vibrato, tremolo;
	PaulaChannel_t samplegen;
}TrackerChannel_t;

typedef struct{
	u8 orders;
	u8 maxpattern;
	u8 order;
	u8 row;
	u8 tick;
	u8 maxtick;
	u8 speed;
	s8 skiporderrequest;
	u8 skiporderdestrow;
	u8 patlooprow;
	u8 patloopcycle;
	u32 audiospeed;
	u32 audiotick;
	u8 fastforward;
	u32 random;
	TrackerChannel_t ch[MOD_CHANNELS];
	Sample_t samples[MAX_SAMPLES];
}ModPlayerStatus_t;

static ModPlayerStatus_t mp;
FATFS fs;
static s16 accum_span[BUFFER_SIZE];
static u8 cony = 2;
static u32 detected_ram;
//static u16 spi_seeks;
u16 oldpad, pad;
u8 play_state = 0;
u8 masterVolume = 64;
u16 total_files;
u8 dirty_sectors = 0;

#define PS_LOADED	1
#define PS_PLAYING	2
#define PS_STOP		4
#define PS_PAUSE	8
#define PS_SHUFFLE	16
#define PS_DRAWN	32

u8 ptime_min,ptime_sec,ptime_frame;

#define PTIME_X	SCREEN_TILES_H-9
#define PTIME_Y	2

static void RecalculateWaveform(Oscillator_t *oscillator);
static void ProcessMOD();
static void RestartMOD();
static void SilenceBuffer();
static void RenderMOD();
static u8 PlayMOD(u8 reload);

static void Intro();
static void UpdateCursor(u8 ylimit);
static void PlayerInterface();
static void InputDeviceHandler();
static void UMPrint(u8 x, u8 y, const char *s);
static void UMPrintRam(u8 x, u8 y, char *s);
static void UMPrintChar(u8 x, u8 y, char c);
static void DrawWindow(u8 x, u8 y, u8 w, u8 h, const char *title, const char *lb, const char *rb);
static void PrintSongTitle(u8 x, u8 y, u8 len);
static u8 IsRootDir();
static void PreviousDir();
static void NextDir(char *s);
static u8 LoadDirData(u8 entry, u8 root);
static void FileSelectWindow();
static void RestartMOD();
static void SavePreferences();
static void LoadPreferences();
static void SpiRamWriteStringEntry(u32 pos, char prefix, char *s);
static void SpiRamWriteStringEntryFlash(u32 pos, const char *s);
static u16 SpiRamStringLen(u32 pos);
static u8 SpiRamPrintString(u8 x, u8 y, u32 pos, u8 invert, u8 fill);
static void SpiRamCopyStringNoBuffer(u32 dst, u32 src, u8 max);
static u8 ButtonHit(u8 x, u8 y, u8 w, u8 h);

#define DEFAULT_COLOR_MASK	0b00000111

static const u8 bad_masks[] PROGMEM = {0,1,2,3,8,9,10,11,16,17,18,19,24,25,26,27,64,65,66,67,72,73,74,75,80,81,82,83,88,89,90,91,};//skip those not legible
static const s32 finetune_table[16] PROGMEM = {
	65536, 65065, 64596, 64132,
	63670, 63212, 62757, 62306,
	69433, 68933, 68438, 67945,
	67456, 66971, 66489, 66011
};

static const u8 sine_table[32] PROGMEM = {
	  0,  24,  49,  74,  97, 120, 141, 161,
	180, 197, 212, 224, 235, 244, 250, 253,
	255, 253, 250, 244, 235, 224, 212, 197,
	180, 161, 141, 120,  97,  74,  49,  24
};

static const s32 arpeggio_table[16] PROGMEM = {
	65536, 61858, 58386, 55109,
	52016, 49096, 46341, 43740,
	41285, 38968, 36781, 34716,
	32768, 30929, 29193, 27554
};