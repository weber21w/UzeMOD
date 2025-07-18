#include "Uzemod.h"


static void RecalculateWaveform(Oscillator_t *oscillator){
	s32 result = 0;
	switch(oscillator->waveform){
		case 0://sine
			result = pgm_read_byte_near(&sine_table[oscillator->phase & 0x1F]);
			if((oscillator->phase & 0x20) > 0) result *= -1;
			break;
		case 1://sawtooth
			result = 255 - (((oscillator->phase + 0x20) & 0x3F) << 3);
			break;
		case 2://square
			result = 255 - ((oscillator->phase & 0x20) << 4);
			break;
		case 3://random
			result = (mp.random >> 20) - 255;
			mp.random = (mp.random * 65 + 17) & 0x1FFFFFFF;
			break;
	}
	oscillator->val = result * oscillator->depth;
}


static void ProcessMOD(){
	if((play_state & PS_PAUSE) || !(play_state & PS_PLAYING))
		return;

	if(++ptime_frame >= 50){
		ptime_frame = 0;
		if(++ptime_sec > 59){
			ptime_sec = 0;
			if(++ptime_min > 99)
				ptime_min = 99;
		}
	}

	if(mp.tick == 0){
		mp.skiporderrequest = -1;
		u8 order_byte = SpiRamReadU8(0, ORDER_TAB_OFF + mp.order);
		u16 row_off = MOD_CHANNELS * (u16)(mp.row + 64 * (u16)order_byte);
		u32 coff = (u32)(PATTERN_BASE + 4 * row_off);
		SpiRamSeqReadStart((u8)(coff >> 16), (u16)(coff & 0xFFFF));//spi_seeks++;
		u8 cell_bytes[4 * MOD_CHANNELS];
		for(u8 i=0; i<sizeof(cell_bytes); i++){
			cell_bytes[i] = SpiRamSeqReadU8();
		}
		SpiRamSeqReadEnd();

		for(u8 ch=0; ch<MOD_CHANNELS; ch++){
			mp.ch[ch].vibrato.val = mp.ch[ch].tremolo.val = 0;
			u8 base = ch * 4;
			u16 note_tmp = (((u16)cell_bytes[base + 0] << 8) | (u16)cell_bytes[base + 1]) & 0x0FFF;
			u8 sample_tmp = (cell_bytes[base + 0] & 0xF0) | (cell_bytes[base + 2] >> 4);
			u8 eff_tmp = cell_bytes[base + 2] & 0x0F;
			u8 effval_tmp = cell_bytes[base + 3];

			if(mp.ch[ch].eff == 0 && mp.ch[ch].effval != 0)
				mp.ch[ch].period = mp.ch[ch].note;

			if(sample_tmp){
				if(sample_tmp > MAX_SAMPLES) sample_tmp = 1;
				mp.ch[ch].sample = sample_tmp - 1;
				Sample_t *smp = &mp.samples[sample_tmp - 1];
				PaulaChannel_t *pch = &mp.ch[ch].samplegen;
				pch->length	= smp->length;
				pch->loopStart = smp->loopStart;
				pch->looplength= smp->looplength;
				pch->spiBase   = smp->spiBase;
				mp.ch[ch].volume = smp->volume;
				//reset pointer on new note
			}
			if(note_tmp){
				u8 finetune;
				if(eff_tmp == 0xE && (effval_tmp & 0xF0) == 0x50){
					finetune = effval_tmp & 0xF;
				}else{
					finetune = mp.samples[mp.ch[ch].sample].finetune;
				}
				note_tmp = (note_tmp * pgm_read_dword_near(&finetune_table[finetune & 0xF])) >> 16;
				mp.ch[ch].note = note_tmp;
				if(eff_tmp != 0x3 && eff_tmp != 0x5 && (eff_tmp != 0xE || (effval_tmp & 0xF0) != 0xD0)){
					PaulaChannel_t *pch = &mp.ch[ch].samplegen;
					pch->currentptr = 0;
					pch->currentsubptr = 0;
					mp.ch[ch].period = mp.ch[ch].note;
					if(mp.ch[ch].vibrato.waveform < 4) mp.ch[ch].vibrato.phase = 0;
					if(mp.ch[ch].tremolo.waveform < 4) mp.ch[ch].tremolo.phase = 0;
				}
			}
			//effect commands...
			if(eff_tmp || effval_tmp){
				switch(eff_tmp){
					case 0x3:
						if(effval_tmp)
							mp.ch[ch].slideamount = effval_tmp;//fallthrough
					case 0x5:
						mp.ch[ch].slidenote = mp.ch[ch].note;
						break;
					case 0x4:
						if(effval_tmp & 0xF0)
							mp.ch[ch].vibrato.speed = effval_tmp >> 4;
						if(effval_tmp & 0x0F)
							mp.ch[ch].vibrato.depth = effval_tmp & 0x0F;//fallthrough
					case 0x6:
						RecalculateWaveform(&mp.ch[ch].vibrato);
						break;
					case 0x7:
						if(effval_tmp & 0xF0)
							mp.ch[ch].tremolo.speed = effval_tmp >> 4;
						if(effval_tmp & 0x0F)
							mp.ch[ch].tremolo.depth = effval_tmp & 0x0F;
						RecalculateWaveform(&mp.ch[ch].tremolo);
						break;
					case 0xC:
						mp.ch[ch].volume = (effval_tmp > 0x40) ? 0x40 : effval_tmp;
						break;
					case 0x9:
						if(effval_tmp){
							mp.ch[ch].samplegen.currentptr = (u32)(effval_tmp << 8);
							mp.ch[ch].sampleoffset = effval_tmp;
						}else{
							mp.ch[ch].samplegen.currentptr = (u32)(mp.ch[ch].sampleoffset << 8);
						}
						break;
					case 0xB:
						if(effval_tmp >= mp.orders)
							effval_tmp = 0;
						mp.skiporderrequest = effval_tmp;
						break;
					case 0xD:
						if(mp.skiporderrequest < 0){
							if(mp.order + 1 < mp.orders)
								mp.skiporderrequest = mp.order + 1;
							else
								mp.skiporderrequest = 0;
						}
						if(effval_tmp > 0x63)
							effval_tmp = 0;
						mp.skiporderdestrow = (effval_tmp >> 4) * 10 + (effval_tmp & 0xF);
						break;
					case 0xE:
						switch(effval_tmp >> 4){
							case 0x1:
								mp.ch[ch].period -= (effval_tmp & 0xF);
								break;
							case 0x2:
								mp.ch[ch].period += (effval_tmp & 0xF);
								break;
							case 0x4:
								mp.ch[ch].vibrato.waveform = (effval_tmp & 0x7);
								break;
							case 0x6:
								if(effval_tmp & 0xF){
									if(!mp.patloopcycle)
										mp.patloopcycle = (effval_tmp & 0xF) + 1;
									if(mp.patloopcycle > 1){
										mp.skiporderrequest = mp.order;
										mp.skiporderdestrow = mp.patlooprow;
									}
									mp.patloopcycle--;
								}else{
									mp.patlooprow = mp.row;
								}
								break;
							case 0x7:
								mp.ch[ch].tremolo.waveform = effval_tmp & 0x7;
								break;
							case 0xA:
								mp.ch[ch].volume += effval_tmp & 0xF;
								if(mp.ch[ch].volume > 0x40)
									mp.ch[ch].volume = 0x40;
								break;
							case 0xB:
								mp.ch[ch].volume -= effval_tmp & 0xF;
								if(mp.ch[ch].volume < 0)
									mp.ch[ch].volume = 0;
								break;
							case 0xE:
								mp.maxtick *= ((effval_tmp & 0xF) + 1);
								break;
						}
						break;
					case 0xF:
						if(effval_tmp){
							if(effval_tmp < 0x20){
								mp.maxtick = ((mp.maxtick / mp.speed) * effval_tmp);
								mp.speed = effval_tmp;
							}else{
								mp.audiospeed = (u32)(SAMPLE_RATE * 125 / effval_tmp / 50);
							}
						}
						break;
				}
			}
			mp.ch[ch].eff = eff_tmp;
			mp.ch[ch].effval = effval_tmp;
		}
	}

	for(u8 ch=0; ch<MOD_CHANNELS; ch++){
		u8 eff_tmp = mp.ch[ch].eff;
		u8 effval_tmp = mp.ch[ch].effval;
		if(eff_tmp || effval_tmp){
			switch(eff_tmp){
				case 0x0:
					switch(mp.tick % 3){
						case 0:
							mp.ch[ch].period = mp.ch[ch].note;
							break;
						case 1:
							mp.ch[ch].period = (mp.ch[ch].note * pgm_read_dword_near(&arpeggio_table[effval_tmp >> 4])) >> 16;
							break;
						case 2:
							mp.ch[ch].period = (mp.ch[ch].note * pgm_read_dword_near(&arpeggio_table[effval_tmp & 0xF])) >> 16;
							break;
					}
					break;
				case 0x1:
					if(mp.tick)
						mp.ch[ch].period -= effval_tmp;
					break;
				case 0x2:
					if(mp.tick)
						mp.ch[ch].period += effval_tmp;
					break;
				case 0x5:
					if(mp.tick){
						if(effval_tmp > 0xF){
							mp.ch[ch].volume += (effval_tmp >> 4);
							if(mp.ch[ch].volume > 0x40)
								mp.ch[ch].volume = 0x40;
						}else{
							mp.ch[ch].volume -= (effval_tmp & 0xF);
							if(mp.ch[ch].volume < 0)
								mp.ch[ch].volume = 0;
						}
					}
					effval_tmp = 0;//fallthrough
				case 0x3:
					if(mp.tick){
						if(!effval_tmp)
							effval_tmp = mp.ch[ch].slideamount;
						if(mp.ch[ch].slidenote > mp.ch[ch].period){
							mp.ch[ch].period += effval_tmp;
							if(mp.ch[ch].slidenote < mp.ch[ch].period)
								mp.ch[ch].period = mp.ch[ch].slidenote;
						}else if(mp.ch[ch].slidenote < mp.ch[ch].period){
							mp.ch[ch].period -= effval_tmp;
							if(mp.ch[ch].slidenote > mp.ch[ch].period)
								mp.ch[ch].period = mp.ch[ch].slidenote;
						}
					}
					break;
				case 0x4:
					if(mp.tick){
						mp.ch[ch].vibrato.phase += mp.ch[ch].vibrato.speed;
						RecalculateWaveform(&mp.ch[ch].vibrato);
					}
					break;
				case 0x6:
					if(mp.tick){
						mp.ch[ch].vibrato.phase += mp.ch[ch].vibrato.speed;
						RecalculateWaveform(&mp.ch[ch].vibrato);
					}//fallthrough
				case 0xA:
					if(mp.tick){
						if(effval_tmp > 0xF){
							mp.ch[ch].volume += (effval_tmp >> 4);
							if(mp.ch[ch].volume > 0x40)
								mp.ch[ch].volume = 0x40;
						}else{
							mp.ch[ch].volume -= (effval_tmp & 0xF);
							if(mp.ch[ch].volume < 0)
								mp.ch[ch].volume = 0;
						}
					}
					break;
				case 0x7:
					if(mp.tick){
						mp.ch[ch].tremolo.phase += mp.ch[ch].tremolo.speed;
						RecalculateWaveform(&mp.ch[ch].tremolo);
					}
					break;
				case 0xE:
					switch(effval_tmp >> 4){
						case 0x9:
							if(mp.tick && !(mp.tick % (effval_tmp & 0xF)))
								mp.ch[ch].samplegen.currentptr = mp.ch[ch].samplegen.currentsubptr = 0;
							break;
						case 0xC:
							if(mp.tick >= (effval_tmp & 0xF))
								mp.ch[ch].volume = 0;
							break;
						case 0xD:
							if(mp.tick == (effval_tmp & 0xF)){
								mp.ch[ch].samplegen.currentptr = mp.ch[ch].samplegen.currentsubptr = 0;
								mp.ch[ch].period = mp.ch[ch].note;
							}
							break;
					}
					break;
			}
		}
		if(mp.ch[ch].period < 0)
			mp.ch[ch].period = 0;
		if(mp.ch[ch].period){
			mp.ch[ch].samplegen.period = PAULA_RATE / (mp.ch[ch].period + (mp.ch[ch].vibrato.val >> 7));
		}else{
			mp.ch[ch].samplegen.period = 0;
		}
		s32 vol = mp.ch[ch].volume + (mp.ch[ch].tremolo.val >> 6);
		if(vol < 0)
			vol = 0;
		else if(vol > 64)
			vol = 64;
		mp.ch[ch].samplegen.volume = vol;
	}

	mp.tick++;
	if(mp.tick >= mp.maxtick){
		mp.tick = 0;
		mp.maxtick = mp.speed;
		if(mp.skiporderrequest >= 0){
			mp.row = mp.skiporderdestrow;
			mp.order = mp.skiporderrequest;
			mp.skiporderdestrow = 0;
			mp.skiporderrequest = -1;
		}else{
			mp.row++;
			if(mp.row >= 0x40){
				mp.row = 0;
				mp.order++;
				if(mp.order >= mp.orders)
					mp.order = 0;
			}
		}
	}
}


static void RestartMOD(){
	mp.row = mp.order	= 0;
	mp.maxtick		= mp.speed = 6;
	mp.audiospeed		= DEFAULT_AUDIO_SPEED;
	mp.audiotick		= mp.audiospeed;
	mp.random		= 1;
	ptime_min		= ptime_sec = ptime_frame = 0;
	for(u8 i=0; i<MOD_CHANNELS; i++)
		mp.ch[i].samplegen.volume = 0;//disable channel
}


static void SilenceBuffer(){
	for(u16 i=0; i<262*2; i++)
		mix_buf[i] = 0x80;
}


static void RenderMOD(){
	PlayerInterface();
	if(!(play_state & PS_DRAWN)){
		ClearVram();
		DrawMap(5,0,buttons_map);
		PrintSongTitle((CONT_BAR_X/8),(CONT_BAR_Y/8)+2,20);
		play_state |= PS_DRAWN;
	}
	if((play_state & PS_PAUSE) || !(play_state & PS_PLAYING)){
		SilenceBuffer();
		return;
	}
	SetRenderingParameters(33, 1);
	u16 offset = 0;
	u16 to_do = SAMPLES_PER_FRAME;
	int8_t *buf = (mix_bank ? (int8_t*)mix_buf + 0 : (int8_t*)mix_buf + 262);

	u8 ffticks = mp.fastforward;
	while(ffticks){
		if(mp.audiotick)
			mp.audiotick = 0;
		ProcessMOD();
		mp.audiotick = mp.audiospeed;
		ffticks--;
	}
	while(to_do > 0){
		if(mp.audiotick == 0){
			ProcessMOD();
			mp.audiotick = mp.audiospeed;
		}
		u16 span_len = (mp.audiotick < to_do) ? mp.audiotick : to_do;
		for(u16 s=0; s<span_len; s++){//zero accumulator for this span
			accum_span[s] = 0;
		}
		for(u8 ch=0; ch<MOD_CHANNELS; ch++){
			if(GetVsyncFlag())
				break;
			PaulaChannel_t *pch = &mp.ch[ch].samplegen;

			if(!pch->spiBase)//skip if no sample or already ended non-loop
				continue;
	if(pch->looplength == 0 && pch->currentptr >= pch->length){//deactivate
		pch->volume = 0;
		pch->spiBase = 0;
		continue;
	}

	u32 localSpiBase   = pch->spiBase;
	u32 localCurrPtr   = pch->currentptr;
	//u32 localLoopStart = pch->loopStart;
	u32 localLength	= pch->length;
	u32 localLoopLen   = pch->looplength;
	u32 localSubPtr	= pch->currentsubptr;
	u32 localPeriod	= pch->period;
	s32 localVolume	= pch->volume;

	if(localVolume <= 0){//deactive
		pch->spiBase = 0;
		continue;
	}

	u32 cur_seq_off = 0;
	u8 seq_active = 0;

	for(u16 s=0; s<span_len; s++){
		if(localCurrPtr >= localLength){//deactivate if at or past end of of non-looping sample
			localVolume = 0;
			localCurrPtr = localLength;
			pch->spiBase = 0;//deactivate so next frames skip it
			break;
		}

		u32 desired = localSpiBase + localCurrPtr;//get sample byte
		if(!seq_active || cur_seq_off != desired){
			u8 desiredBank = (u8)(desired >> 16);
			u8 curBank = (u8)(cur_seq_off >> 16);
			u32 diff = (desired > cur_seq_off) ? (desired - cur_seq_off) : 0xFFFFFFFF;
			if(seq_active && desiredBank == curBank && diff < 5){
				for(u8 i=0; i<diff; i++) SpiRamSeqReadU8();
			}else{
				if(seq_active)
					SpiRamSeqReadEnd();
				SpiRamSeqReadStart((u8)(desired>>16), (u16)(desired&0xFFFF));
				//spi_seeks++;
			}
			cur_seq_off = desired;
			seq_active = 1;
		}
		u8 rawByte = SpiRamSeqReadU8();
		cur_seq_off++;
		accum_span[s] += (s32)(int8_t)rawByte * localVolume;

		localSubPtr += localPeriod;//advance fractional pointer
		if(localSubPtr >= 0x10000UL){
			u32 step = (u32)(localSubPtr >> 16);
			localCurrPtr += step;
			localSubPtr &= 0xFFFF;
			if(localLoopLen > 0){
			/*
				if(localCurrPtr >= localLength){
					u32 overflow = localCurrPtr - localLength;
					localCurrPtr = localLoopStart + (overflow % localLoopLen);
				}
			*/
				while(localCurrPtr >= localLength)
					localCurrPtr -= localLoopLen;
			}else if(localCurrPtr >= localLength){
				localVolume = 0;
				localCurrPtr = localLength;
				pch->spiBase = 0;//deactivate
				break;
			}
		}
	}
	if(seq_active){
		SpiRamSeqReadEnd();
		seq_active = 0;
	}

	pch->currentptr	= localCurrPtr;//write back updated state...
	pch->currentsubptr = localSubPtr;
	pch->period		= localPeriod;
	pch->volume		= localVolume;
}

		if(masterVolume == 64){
			for(u16 s=0; s<span_len; s++){//write output

				int32_t mixed = accum_span[s] >> 8;
				if(mixed > 127)
					mixed = 127;
				else if(mixed < -128)
					mixed = -128;
				int8_t signedOut = (int8_t)mixed;
				buf[offset + s] = (uint8_t)(signedOut + 128);

			}
		}else{
			for(u16 s=0; s<span_len; s++){
				s32 scaled = (accum_span[s] * (int32_t)masterVolume) >> 14;//8+6
				if(scaled > 127) scaled = 127;
				else if(scaled < -128) scaled = -128;
				int8_t signedOut = (int8_t)scaled;
				buf[offset + s] = (uint8_t)(signedOut + 128);
			}
		}
		offset += span_len;
		to_do -= span_len;
		mp.audiotick -= span_len;
	}
	UMPrint(PTIME_X,PTIME_Y,PSTR("  :  :  "));
	//spi_seeks = 0;
	PrintByte(PTIME_X+8,PTIME_Y,ptime_frame<<1,1);
	PrintByte(PTIME_X+5,PTIME_Y,ptime_sec,1);
	PrintByte(PTIME_X+2,PTIME_Y,ptime_min,0);
	UMPrintChar(PTIME_X+3,PTIME_Y,':');

	UMPrintChar(PTIME_X+6,PTIME_Y,':');
	SetRenderingParameters(33, 24);
}


u8 PlayMOD(u8 reload){
	u8 *as8 = (u8 *)accum_span;
	if(!reload)
		ptime_min = ptime_sec = ptime_frame = 0;
	SetRenderingParameters(33, 10*8);
	cony = 1;
	DrawWindow(1, 0, 28,8,NULL,NULL,NULL);
	SpiRamReadInto(0,0,as8,32);//get file to open(GUI places the selection at 0)
	//SpiRamCopyStringNoBuffer(detected_ram-64, 0, 64);//save the filename, in case we later cancel in GUI

	FRESULT res;
	
	for(u8 i=0; i<3; i++){
		res =	pf_open((char *)as8);
		if(!res){
			pf_lseek(0);
			break;
		}
		WaitVsync(30);
	}
	if(res){
		UMPrint(3,cony++,PSTR("ERROR: File not found"));
		u8 slen = 0;
		for(; slen<26; slen++){
			if(as8[slen] == 0){
				as8[slen] = ']';
				break;
			}
			if(slen == 26-1){
				as8[23] = as8[24] = as8[25] = '.';
			}
		}
		UMPrintChar(3,cony,'[');
		UMPrintRam(4,cony++,(char *)as8);
		goto INIT_FAIL;		
	}

	if(!reload){
		UMPrint(3,cony,PSTR("Loading: "));
		UMPrintRam(12,cony++,(char *)as8);
	}
//	SetRenderingParameters(33, cony*TILE_HEIGHT);
	WORD br;
	for(u8 i=0; i<3; i++){//load header
		pf_read(as8,512,&br);
		while(rcv_spi() != 0xFF);//wait for MISO idle
		if(br != 512){
			PrintByte(30,cony,br,1);
			UMPrint(3, cony++, PSTR("ERROR: header read failed"));
			goto INIT_FAIL;
		}
		u32 base = MOD_BASE + (u32)i * 512UL;
		SpiRamWriteFrom((u8)(base>>16), (u16)(base&0xffff0), as8, br);
		if(dirty_sectors)//for reloads
			dirty_sectors--;
	}
	if(reload && !dirty_sectors)//done, file GUI only corrupted the first bytes of SPI RAM...
		return 0;

	SpiRamSeqReadStart((u8)((MOD_BASE + 1080UL) >> 16), (u16)((MOD_BASE + 1080UL) & 0xFFFF));
	u8 sig0 = SpiRamSeqReadU8();
	u8 sig1 = SpiRamSeqReadU8();
	u8 sig2 = SpiRamSeqReadU8();
	u8 sig3 = SpiRamSeqReadU8();
	SpiRamSeqReadEnd();
	u32 signature = ((u32)sig0 << 24) | ((u32)sig1 << 16) | ((u32)sig2 << 8) | (u32)sig3;
	UMPrint(3, cony, PSTR("Signature:\"    \""));
	UMPrintChar(14, cony, sig0);
	UMPrintChar(15, cony, sig1);
	UMPrintChar(16, cony, sig2);
	UMPrintChar(17, cony++, sig3);
	SetRenderingParameters(33, cony*TILE_HEIGHT);
	if(signature != 0x4D2E4B2E && signature != 0x214B214D){//supported format "M.K" or "M!K!"?
		UMPrint(3, cony++, PSTR("ERROR: bad signature"));
		goto INIT_FAIL;
	}

	memset(&mp, 0, sizeof(mp));
	mp.orders = SpiRamReadU8((MOD_BASE + 950UL) >> 16, (MOD_BASE + 950UL) & 0xFFFF);//get order count
	mp.maxpattern = 0;

	u32 poff = MOD_BASE + ORDER_TAB_OFF;
	SpiRamSeqReadStart((u8)(poff >> 16), (u16)(poff & 0xFFFF));
	for(u8 i=0; i<128; i++){//determine maxpattern from order table
		u8 t = SpiRamSeqReadU8();
		if(t >= mp.maxpattern) mp.maxpattern = t;
	}
	SpiRamSeqReadEnd();
	mp.maxpattern++;
	UMPrint(3, cony, PSTR("Patterns: "));
	PrintByte(14, cony++, mp.maxpattern, 0);
//	SetRenderingParameters(33, cony*TILE_HEIGHT);

	u32 first_sample_offset = MOD_BASE + 1084UL + (64U * 4UL * MOD_CHANNELS * mp.maxpattern);
	u32 sample_data_last_byte = first_sample_offset;
	u8 used_samples = 0;
	u32 sample_bytes = 0;
	for(u8 i=0; i<MAX_SAMPLES; i++){//scan sample headers to determine total file size
		u32 soff = SAMPLE_HEADER_BASE + (u32)i * SAMPLE_HEADER_SIZE + 22;//offset to length hi byte
		SpiRamSeqReadStart((u8)(soff >> 16), (u16)(soff & 0xFFFF));
		u16 lenw = (u16)(SpiRamSeqReadU8() << 8) | SpiRamSeqReadU8();
		if(lenw)
		used_samples++;
		sample_bytes += (u16)(lenw << 1);
		sample_data_last_byte += (u16)(lenw << 1);
		SpiRamSeqReadEnd();
		if(sample_data_last_byte >= detected_ram-512){//last 512 bytes are reserved for the filename of the currently loaded song, current directory, etc.
			UMPrint(3, cony++, PSTR("ERROR: insufficient memory"));
			goto INIT_FAIL;
		}
	}
	UMPrint(3, cony, PSTR("Samples count: "));
	PrintByte(19, cony++, used_samples, 0);
	PrintLong(21, cony, sample_bytes);
	UMPrint(3, cony++, PSTR("Samples bytes: "));
	PrintLong(18 + (sample_data_last_byte > 100000UL ? 1 : 0), cony, sample_data_last_byte);

	UMPrint(3, cony, PSTR("Title: "));
	PrintSongTitle(10,cony,19);//make sure we don't write over the window edge...
	cony++;
	SetRenderingParameters(33, (cony+2)*TILE_HEIGHT);

	u32 total_sectors = (sample_data_last_byte + 511) / 512;
	PrintInt(26, cony, total_sectors, 0);
	UMPrintChar(23, cony, '/');
	UMPrint(3, cony, PSTR("Loading sector"));
	for(u16 i=3; i<total_sectors; i++){//load remaining sectors into SPI RAM
		PrintInt(22, cony, i+1, 0);
		if(i & 2) UMPrintChar(19, cony, '.');
		if(i & 4) UMPrintChar(18, cony, '.');
		if(i & 8) UMPrintChar(17, cony, '.');
		else	  UMPrintChar(17, cony, ' ');

		pf_read(as8,512,&br);
		while(rcv_spi() != 0xFF);//wait for MISO idle
		u32 base = MOD_BASE + (u32)i * 512UL;
		SpiRamWriteFrom((u8)(base>>16), (u16)(base&0xffff0), as8, br);
		SpiRamSeqReadEnd();
		if(dirty_sectors)
			dirty_sectors--;
		else if(reload)//fixed all sectors corrupted by GUI?
			return 0;
	}
	cony++;

	u32 current_data_offset = first_sample_offset;
	u32 hdr_base = MOD_BASE + 20;
	SpiRamSeqReadStart((u8)(hdr_base >> 16), (u16)(hdr_base & 0xFFFF));
	ClearVram();
	for(u8 i=0; i<MAX_SAMPLES; i++){//load sample headers into mp.samples[]
		for(u8 j=0; j<22; j++){//skip sample name
			SpiRamSeqReadU8();
		}

		u8 hi = SpiRamSeqReadU8();//read length...
		u8 lo = SpiRamSeqReadU8();//...in words
		u16 length_words = ((u16)hi << 8) | (u16)lo;

		u8 finetune = SpiRamSeqReadU8();
		u8 volume = SpiRamSeqReadU8();

		hi = SpiRamSeqReadU8();//read repeat point...
		lo = SpiRamSeqReadU8();//...in words
		u16 repeat_point = ((u16)hi << 8) | (u16)lo;

		hi = SpiRamSeqReadU8();//read repeat length...
		lo = SpiRamSeqReadU8();//...in words
		u16 repeat_length = ((u16)hi << 8) | (u16)lo;

		u32 length_bytes = (u32)length_words * 2;
		u32 loop_start_bytes = (u32)repeat_point * 2;
		u32 loop_length_bytes;

		if(repeat_length < 2){//no loop
			loop_length_bytes = 0;
			loop_start_bytes = 0;
		}else{
			loop_length_bytes = (u32)repeat_length * 2;
		}
		//store into mp.samples
		mp.samples[i].spiBase	=	current_data_offset;
		mp.samples[i].length	=	length_bytes;
		mp.samples[i].loopStart	=	loop_start_bytes;
		mp.samples[i].looplength=	loop_length_bytes;
		mp.samples[i].finetune	=	finetune;
		mp.samples[i].volume	=	volume;
		current_data_offset += length_bytes;//advance for next sample
	}
	SpiRamSeqReadEnd();

	mp.maxtick	= mp.speed = 6;
	mp.audiospeed	= DEFAULT_AUDIO_SPEED;
	mp.audiotick	= mp.audiospeed;
	mp.random	= 1;
	play_state = PS_LOADED|PS_PLAYING;
	return 0;

INIT_FAIL:
	play_state = 0;
	SpiRamWriteU8((u8)((u32)(detected_ram-64)>>16),(u16)((u32)(detected_ram-64)&0xFFFF),0);//erase previous filename to avoid GUI cancel/reload
	SetRenderingParameters(33, SCREEN_TILES_V*TILE_HEIGHT);
	WaitVsync(240);
	return 1;
}


void Intro(){
return;
	UMPrint(3,8,PSTR("Uzemod - NWCM Demo Edition"));
	FadeIn(4,1);
	WaitVsync(120);
	FadeOut(3,1);
	FadeIn(1,0);
}


int main(){
	SetFontTilesIndex(0);
	SetTileTable(tile_data);
	SetSpritesTileTable(tile_data);
	ClearVram();
	Intro();
	DrawWindow(1, 1, 28,10,NULL,NULL,NULL);
	do{
		WaitVsync(1);//let fade in finish before we load the skin..
	}while(DDRC != 255);
	LoadPreferences();
#if (ENABLE_MIXER == 1)
	SetMasterVolume(255);
#endif
	//WORD	br;
	FRESULT res;
	u8 i;
	for(i=0; i<10 ; i++){
		res = pf_mount(&fs);
		if(!res){
			UMPrint(3,cony++,PSTR("Mounted SD Card"));
			i = 0;
			break;
		}
	}
	if(i){
		PrintByte(20,cony,res,0);
		UMPrint(3,cony++,PSTR("ERROR: SD Mount:"));
		goto MAIN_FAIL;
	}

	u8 bank_count = SpiRamInit();

	if(!bank_count){
		UMPrint(3,cony++,PSTR("ERROR: No SPI RAM detected"));
		goto MAIN_FAIL;
	}else{
		bank_count = 2;//BANK HACK
		detected_ram = (u32)(bank_count*(64UL*1024UL));
		u8 moff = 21;		//>=65536
		if(bank_count > 1)	//>=131072
			moff++;
		if(bank_count > 15)	//>=1048576
			moff++;

		PrintLong(moff,cony,(u32)detected_ram/1024UL);
		UMPrintChar(moff+1,cony,'K');
		UMPrint(3,cony++,PSTR("SPI RAM Detected:"));
		WaitVsync(60);
	}

	SpiRamWriteStringEntryFlash(0, PSTR("Select File ^"));
	NextDir(NULL);
	SpiRamPrintString(4,0,(u32)(detected_ram-512),0,4);
	SetRenderingParameters(33,24);

	sprites[0].tileIndex = TILE_CURSOR;
	sprites[0].flags = sprites[0].x = sprites[0].y = 0;

	while(1){
		WaitVsync(1);
		RenderMOD();
	}
MAIN_FAIL:
	WaitVsync(240);
	SoftReset();
	return 0;
}


static void InputDeviceHandler(){//requires the kernel to *NOT* read controllers during VSYNC
	u8 i;
	joypad1_status_lo = joypad2_status_lo = joypad1_status_hi = joypad2_status_hi = 0;

	JOYPAD_OUT_PORT |= _BV(JOYPAD_LATCH_PIN);//latch controllers
	for(i=0;i<8+1;i++)//7 seems to glitch
		Wait200ns();
	JOYPAD_OUT_PORT&=~(_BV(JOYPAD_LATCH_PIN));//unlatch controllers

	for(i=0; i<16; i++){//read button states from the shift registers 
		joypad1_status_lo >>= 1;
		joypad2_status_lo >>= 1;

		JOYPAD_OUT_PORT &= ~(_BV(JOYPAD_CLOCK_PIN));//pulse clock pin

		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA1_PIN)) == 0)
			joypad1_status_lo |= (1<<15);
		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA2_PIN)) == 0)
			joypad2_status_lo |= (1<<15);
		
		JOYPAD_OUT_PORT |= _BV(JOYPAD_CLOCK_PIN);
		for(uint8_t j=0;j<33+1;j++)//32 seems to glitch?
			Wait200ns();
	}

	if(joypad1_status_lo == (BTN_START+BTN_SELECT+BTN_Y+BTN_B) || joypad2_status_lo == (BTN_START+BTN_SELECT+BTN_Y+BTN_B))
		SoftReset();
	
	for(i=0; i<8+1; i++)//wait 1.6us, any less and it glitches
		Wait200ns();

	for(i=0; i<16; i++){//Read extended mouse data on both ports(it's fine if there is no mouse there)
		joypad1_status_hi <<= 1;
		joypad2_status_hi <<= 1;
		JOYPAD_OUT_PORT &= ~(_BV(JOYPAD_CLOCK_PIN));//pulse clock pin(no delay required on Hyperkin)

		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA1_PIN)) == 0)
			joypad1_status_hi |= 1;
		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA2_PIN)) == 0)
			joypad2_status_hi |= 1;

		JOYPAD_OUT_PORT |= _BV(JOYPAD_CLOCK_PIN);
		for(uint8_t j=0;j<33+1;j++)//32 seems to glitch?
			Wait200ns();
	}
}


static void DrawWindow(u8 x, u8 y, u8 w, u8 h, const char *title, const char *lb, const char *rb){
	SetTile(x+0,y+0,TILE_WIN_TLC);
	SetTile(x+w,y+0,TILE_WIN_TRC);
	SetTile(x+0,y+h,TILE_WIN_BLC);
	SetTile(x+w,y+h,TILE_WIN_BRC);
	for(u8 y2=y+1; y2<y+h; y2++){
		for(u8 x2=x+1; x2<(x+w); x2++){
			SetTile(x2, y2, 0);
		}
	}
	for(u8 x2=x+1; x2<x+w; x2++){
		SetTile(x2,y,TILE_WIN_TBAR);
		SetTile(x2,y+h,TILE_WIN_BBAR);
	}
	for(u8 y2=y+1; y2<y+h; y2++){
		SetTile(x,y2,TILE_WIN_LBAR);
		SetTile(x+w,y2,TILE_WIN_RBAR);
	}
	if(title != NULL)
		UMPrint(x+1,y,title);
	if(lb != NULL)
		UMPrint(x+1,y+h,lb);
	if(rb != NULL){
		u8 xo = x+w;
		for(u8 i=0; i<16; i++){
			if(pgm_read_byte(rb[i]) == '\0')
				break;
			xo--;
		}
		UMPrint(xo,y+h,rb);
	}
}


static void UpdateCursor(u8 ylimit){
	InputDeviceHandler();
	oldpad = pad;
	pad = ReadJoypad(0);
	u8 speed;
	if(pad & BTN_SR)
		speed = 1;
	else
		speed = 2;

	if((pad & BTN_LEFT)){
		if(sprites[0].x < speed)
			sprites[0].x = 0;
		else
			sprites[0].x -= speed;
	}else if((pad & BTN_RIGHT)){
		if(sprites[0].x+speed > (SCREEN_TILES_H*TILE_WIDTH)-9)
			sprites[0].x = (SCREEN_TILES_H*TILE_WIDTH)-9;
		else
			sprites[0].x += speed;
	}

	if((pad & BTN_UP)){
		if(sprites[0].y < speed)
			sprites[0].y = 0;
		else
			sprites[0].y -= speed;
	}else if((pad & BTN_DOWN)){
		if(sprites[0].y+8+speed > ylimit+3)
			sprites[0].y = ylimit-8+3;
		else
			sprites[0].y += speed;
	}

	for(u8 i=0; i<2; i++){
		u16 p = ReadJoypad(i);
		if(!(p & MOUSE_SIGNATURE))
			continue;

		u8 xsign,ysign;
		u16 deltax,deltay;
		if(i == 0){
			deltax = (joypad1_status_hi & 0b0000000001111111);
			deltay = (joypad1_status_hi & 0b0111111100000000)>>8;
			xsign = (joypad1_status_hi & 0b1000000000000000)?1:0;
			ysign = (joypad1_status_hi & 0b0000000010000000)?1:0;
		}else{
			deltax = (joypad2_status_hi & 0b0000000001111111);
			deltay = (joypad2_status_hi & 0b0111111100000000)>>8;
			xsign = (joypad2_status_hi & 0b1000000000000000)?1:0;
			ysign = (joypad2_status_hi & 0b0000000010000000)?1:0;
		}
		if(xsign){//right mouse movement
			deltax = -deltax;
			(u16)(deltax += sprites[0].x);
			(u16)(sprites[0].x = (deltax < 0)?0:deltax);
		}else{//left mouse movement
			(u16)(deltax += sprites[0].x);
			(u16)(sprites[0].x = (deltax > (SCREEN_TILES_H*TILE_WIDTH)-8)?(SCREEN_TILES_H*TILE_WIDTH)-8:deltax);
		}
		if(ysign){//up mouse movement
			deltay = -deltay;
			(u16)(deltay += sprites[0].y);
			(u16)(sprites[0].y = (deltay < 0)?0:deltay);
		}else{//down mouse movement
			(u16)(deltay += sprites[0].y);
			(u16)(sprites[0].y = (deltay > ylimit-4)?(ylimit-4):deltay);
		}
	}
}


static void PlayerInterface(){
	UpdateCursor(24);

	u8 btn,newclick=0;
	if(sprites[0].y >= (CONT_BAR_Y+CONT_BTN_H) || sprites[0].x < CONT_BAR_X || sprites[0].x >= CONT_BAR_X+CONT_BAR_W)
		btn = 255;
	else
		btn = (sprites[0].x-CONT_BAR_X)/CONT_BTN_W;

	static u8 lastbtn = 255;
	if((pad & (BTN_Y|BTN_MOUSE_LEFT)) && !(oldpad & (BTN_Y|BTN_MOUSE_LEFT))){
		lastbtn = btn;
		newclick = 1;
	}

	if(lastbtn != 255){
		u8 moff = 2+(lastbtn*2);
		u8 xoff = (CONT_BAR_X/8)+(lastbtn*(CONT_BTN_W/8));
		if((pad & BTN_Y)){//still holding?	
			SetTile(xoff+0,(CONT_BAR_Y/8)+0,pgm_read_byte(&pressed_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+0,pgm_read_byte(&pressed_map[moff]));
			moff += (CONT_BAR_W/TILE_WIDTH)-1;
			SetTile(xoff+0,(CONT_BAR_Y/8)+1,pgm_read_byte(&pressed_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+1,pgm_read_byte(&pressed_map[moff]));
		}else{//released
			SetTile(xoff+0,(CONT_BAR_Y/8)+0,pgm_read_byte(&buttons_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+0,pgm_read_byte(&buttons_map[moff]));
			moff += (CONT_BAR_W/TILE_WIDTH)-1;
			SetTile(xoff+0,(CONT_BAR_Y/8)+1,pgm_read_byte(&buttons_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+1,pgm_read_byte(&buttons_map[moff]));
			lastbtn = 255;
		}
	}
	mp.fastforward = 0;
	if(newclick){
		if(play_state & PS_LOADED){
			if(btn == 0){//previous

			}else if(btn == 1){//restart
				RestartMOD();
			}else if(btn == 2){//pause
				if(play_state & PS_PAUSE)
					play_state ^= PS_PAUSE;
				else
					play_state = PS_LOADED|PS_PAUSE;
			}else if(btn == 3){//play
				play_state = PS_LOADED|PS_DRAWN|PS_PLAYING;
			}else if(btn == 4){//fast forward
				play_state = PS_LOADED|PS_DRAWN|PS_PLAYING;//eliminate pause if present
			}else if(btn == 5){//next

			}
		}
		if(btn == 6){
			WaitVsync(1);
			FileSelectWindow();
			pad = oldpad = 0b0000111111111111;
			WaitVsync(1);
			play_state &= ~PS_DRAWN;//force redraw
		}else if(btn == 7){//blank

		}else if(btn == 8){//volume down
			if(masterVolume)
				masterVolume--;
		}else if(btn == 9){//volume up
			masterVolume++;
			if(masterVolume > 64)
				masterVolume = 64;
		}else if(btn == 10){//skin prev
			DDRC = DDRC-1;
			u8 bad = 1;
			while(bad){
				bad = 0;
				for(u8 i=0; i<sizeof(bad_masks); i++){
					if(DDRC == pgm_read_byte(&bad_masks[i])){
						bad = 1;
						DDRC = DDRC-1;
						break;
					}
				}
			}
		}else if(btn == 11){//skin next
			DDRC = DDRC+1;
			u8 bad = 1;
			while(bad){
				bad = 0;
				for(u8 i=0; i<sizeof(bad_masks); i++){
					if(DDRC == pgm_read_byte(&bad_masks[i])){
						bad = 1;
						DDRC = DDRC+1;
						break;
					}
				}
			}
		}else if(btn == 12){//save pref
			SavePreferences();
		}
	}
	if(lastbtn != 255){
		if(lastbtn == 1){//fast backward
			//fast_backward = 2;
		}else if(lastbtn == 4){//fast forward
			mp.fastforward = 2;
		}
	}
}


static void LoadPreferences(){
	struct EepromBlockStruct ebs;
	ebs.id = UZENET_EEPROM_ID1;//use Uzenet color preference
	if(EepromReadBlock(ebs.id, &ebs) == 0){
		DDRC = ebs.data[29];
		//masterVolume = ebs.data[28];
		//if(masterVolume < 16 || masterVolume > 64)
		//masterVolume = 64;
	}else{
		DDRC = DEFAULT_COLOR_MASK;
		masterVolume = 64;
	}
}


static void SavePreferences(){
	struct EepromBlockStruct ebs;
	ebs.id = UZENET_EEPROM_ID1;

	if(EepromReadBlock(ebs.id, &ebs)){//doesn't exist, try to make it
		for(uint8_t i=0;i<30;i++)
			ebs.data[i] = 0;
	//	ebs.data[28] = 64;//default master volume
		ebs.data[29] = DDRC;
		EepromWriteBlock(&ebs);
	}

	if(EepromReadBlock(ebs.id, &ebs) == 0){
		ebs.data[29] = DDRC;
		EepromWriteBlock(&ebs);
	}
}


static void UMPrintChar(u8 x, u8 y, char c){
	if(c >= 'a')
		c -= 32;
	SetTile(x,y,(c-32));
}


static void UMPrint(u8 x, u8 y, const char *s){
	u8 soff = 0;
	do{
		char c = pgm_read_byte(&s[soff++]);
		if(c == '\0')
			break;
		//if(c >= 'a')
		//	c -= 32;
		UMPrintChar(x++,y,c);
	}while(1);
}


static void UMPrintRam(u8 x, u8 y, char *s){
	u8 soff = 0;
	do{
		char c = s[soff++];
		if(c == '\0')
			break;
		//if(c >= 'a')
		//	c -= 32;
		UMPrintChar(x++,y,c);
	}while(1);
}


static void PrintSongTitle(u8 x, u8 y, u8 len){
	SpiRamSeqReadStart((u8)(MOD_BASE >> 16), (u16)(MOD_BASE & 0xFFFF));
	u8 i=0;
	for(i=0; i<len; i++){
		u8 c = SpiRamSeqReadU8();
		if(c == 0)
			break;
		UMPrintChar(x + i, y, c);
	}
	SpiRamSeqReadEnd();
	for(; i<len; i++){
		SetTile(x+i,y,0);
	}
}


static u8 IsRootDir(){
	u32 base = (u32)(detected_ram-512)+1;//assumes first current dir character is always '/'
	u8 c = SpiRamReadU8((u8)(base>>16),(u16)(base&0xFFFF));
	return (c == 0)?1:0;
}


static void PreviousDir(){
	u32 base = (u32)(detected_ram-512);
	u16 slen = SpiRamStringLen(base);
	u32 last_slash = 0;
	SpiRamSeqReadStart((u8)(base>>16),(u16)(base&0xFFFF));
	for(u16 i=0; i<slen-1; i++){
		if(SpiRamSeqReadU8() == '/')
			last_slash = i;
	}
	SpiRamSeqReadEnd();
	if(last_slash == 0)
		last_slash = 1;
	base += (u32)last_slash;
	SpiRamWriteU8((u8)(base>>16),(u16)(base&0xFFFF),0);
}


static void NextDir(char *s){
	u32 base = (u32)(detected_ram-512);
	if(s == NULL){
		SpiRamWriteStringEntryFlash((u32)base, PSTR("/"));
		return;
	}
	base += (u32)SpiRamStringLen((u32)base);
	u8 isroot = IsRootDir();
	SpiRamSeqWriteStart((u8)(base>>16),(u16)(base&0xFFFF));
	if(!isroot)
		SpiRamSeqWriteU8('/');
	while(1){
		u8 c = s[0];
		s++;
		if(c == 0)
			break;
		SpiRamSeqWriteU8(c);
	}
	//SpiRamSeqWriteU8('/');
	SpiRamSeqWriteEnd();
}


static u8 LoadDirData(u8 entry, u8 root){
	total_files = 0;
	u8 *as8 = (u8 *)accum_span;

	SpiRamReadInto(0,0,as8,64);//save last loaded song, if any
	u32 base = (u32)(detected_ram-64);
	SpiRamWriteFrom((u8)(base>>16),(u16)(base&0xFFFF),as8,64);

	base = (u32)(entry*32UL);
	SpiRamReadInto((u8)(base>>16),(u16)(base&0xFFFF),as8,32);//load selected filename from GUI
	base = (u32)(detected_ram-512);
	SpiRamReadInto((u8)(base>>16),(u16)(base&0xFFFF),as8,256);//load current directory
	FRESULT res;
	FILINFO fno;
	DIR dir;
	res = pf_opendir(&dir, (char *)as8);
	if(res == FR_OK){
		while(1){
			res = pf_readdir(&dir, &fno);
			if(res != FR_OK || fno.fname[0] == 0)
				break;

			if((fno.fattrib & AM_DIR)){//directory?
				SpiRamWriteStringEntry((u32)(total_files*32), '/', fno.fname);
				total_files++;
			}else{//file, make sure it's a mod
				u8 valid = 0;
				for(u8 i=0; i<13-3; i++){
					if(fno.fname[i+0] == 0)
						break;
					if((fno.fname[i+0] == '.') && (fno.fname[i+1] == 'M') && (fno.fname[i+2] == 'O') && (fno.fname[i+3] == 'D')){
						valid = 1;
						break;
					}
				}
				if(valid){
					SpiRamWriteStringEntry((u32)(total_files*32), 0, fno.fname);
					total_files++;
				}
			}
		}
		dirty_sectors = 1+((total_files*64)/512);
		return 0;
	}
	dirty_sectors = 1+((total_files*64)/512);
	return res;
}


static void SpiRamWriteStringEntryFlash(u32 pos, const char *s){
	u8 i;
	SpiRamSeqWriteStart((u8)(pos>>16),(u16)(pos&0xFFFF));//file first entry so we can load the directory
	for(i=0; i<32; i++){
		char c = pgm_read_byte(s++);
		SpiRamSeqWriteU8(c);
		if(c == '\0')
			break;
	}
	for(; i<32; i++)
		SpiRamSeqWriteU8('\0');

	SpiRamSeqWriteEnd();
}


static void SpiRamWriteStringEntry(u32 pos, char prefix, char *s){
	u8 i;
	SpiRamSeqWriteStart((u8)(pos>>16),(u16)(pos&0xFFFF));//file first entry so we can load the directory
	if(prefix)
		SpiRamSeqWriteU8(prefix);
	for(i=0; i<32; i++){
		char c = *s++;
		SpiRamSeqWriteU8(c);
		if(c == '\0')
			break;
	}
	for(; i<32; i++)
		SpiRamSeqWriteU8('\0');

	SpiRamSeqWriteEnd();
}


static u16 SpiRamStringLen(u32 pos){
	SpiRamSeqReadStart((u8)(pos>>16),(u16)(pos&0xFFFF));
	u16 len = 0;
	while(SpiRamSeqReadU8() != 0 && len < 4096)
		len++;
	SpiRamSeqReadEnd();
	return len;
}


static u8 SpiRamPrintString(u8 x, u8 y, u32 pos, u8 invert, u8 fill){
	//u16 voff = (VRAM_TILES_H*y)+x;
	SpiRamSeqReadStart((u8)(pos>>16),(u16)(pos&0xFFFF));
	u8 ret = 255;
	while(1){
		char c = SpiRamSeqReadU8();
		if(ret == 255){
			ret = (c == '/')?1:0;//is directory?
		}
		if(c == '\0'){
			while(fill){
				SetTile(x++,y,0);
				fill--;
			}
			break;
		}
		if(invert)
			c += 64;
		if(fill)
			fill--;
		UMPrintChar(x++,y,c);
	}
	SpiRamSeqReadEnd();
	return ret;
}


static u8 ButtonHit(u8 x, u8 y, u8 w, u8 h){
	if(sprites[0].x < (x<<3) || sprites[0].x > (x<<3)+(w<<3) || sprites[0].y < (y<<3) || sprites[0].y > (y<<3)+(h<<3))
		return 0;
	return 1;
}


static void SpiRamCopyStringNoBuffer(u32 dst, u32 src, u8 max){
	while(max--){
		char c = SpiRamReadU8(0, (u16)(src&0xFFFF));
		SpiRamWriteU8((u8)(dst>>16), (u16)(dst&0xFFFF), c);
		if(!c)
			return;
		src++;
		dst++;
	}
	dst--;
	SpiRamWriteU8(0, (u16)(dst&0xFFFF),'\0');
}


static void FileSelectWindow(){
	SilenceBuffer();
	ClearVram();
	SetRenderingParameters(33, SCREEN_TILES_V*TILE_HEIGHT);
	//for(u16 i=64; i<VRAM_SIZE; i++)
	//	vram[i] = RAM_TILES_COUNT;

	u8 layer = 1;
	u8 loaded_dir = 0;
	u8 notroot = 0;
	u16 foff = 0;
	u8 lastclick = 255;
	u8 last_line = 255;
	while(1){
		WaitVsync(1);
		UpdateCursor(SCREEN_TILES_V*TILE_HEIGHT);
		u8 line = sprites[0].y/8;
		u8 click = 0;
		if(lastclick < 20)
			lastclick++;
		if(pad & (BTN_Y|BTN_SL|BTN_SR|BTN_MOUSE_LEFT) && !(oldpad & (BTN_Y|BTN_SL|BTN_SR|BTN_MOUSE_LEFT)))
			click = 1;

		if(layer == 0){
			DrawWindow(8,2,14,5,PSTR("Open File"),PSTR("Cancel"),NULL);
			UMPrint(10,4,PSTR("SD Card(L)"));
			UMPrint(10,5,PSTR("Network(R)"));
			if(line == 4 || line == 5){
				for(u8 i=9; i<8+14; i++)
					vram[(line*VRAM_TILES_H)+i] += 64;
			}

			if(click){
				if(pad & (BTN_SL|BTN_SR)){
					layer = (pad&BTN_SL)?1:2;
				}else if(line == 7 && sprites[0].x > 9*8 && sprites[0].x < 15*8){//cancel
					goto FILE_SELECT_END;
				}else if(line == 4){//SD card
					layer = 1;
				}else if(line == 5){//network
					layer = 2;
				}
			}
		}else if(layer == 1){//SD load
			u8 *as8 = (u8 *)accum_span;
			DrawWindow(4,2,22,SCREEN_TILES_V-3,PSTR("Select File"),PSTR("Cancel"),NULL);//was -6
			u8 fline = 0;
			if(line < 4)
				fline = (foff)?foff+1:0;
			else if(line > 3 && line < 4+10 && (foff+(line-4)) < total_files)
				fline = (foff+(line-3));
			else
				fline = ((foff+10)>total_files)?total_files:foff+10;
			PrintInt(16,SCREEN_TILES_V-1,fline,1);//was -4
			PrintInt(25,SCREEN_TILES_V-1,total_files,1);
			UMPrint(18,SCREEN_TILES_V-1,PSTR("of"));
			SetTile(26,2,TILE_WIN_SCRU);
			SetTile(26,SCREEN_TILES_V-1,TILE_WIN_SCRD);
			if(!loaded_dir){
				LoadDirData(0,0);
				loaded_dir = 1;
				notroot = !IsRootDir();
				UMPrint(0,0,PSTR("Dir: "));
				SpiRamPrintString(5,0,(u32)(detected_ram-512),0,SCREEN_TILES_H-6);//current path
			}
			UMPrint(5,3,notroot?PSTR(".."):PSTR("."));

			for(u16 i=0; i<10; i++){
				if(foff+i >= total_files)
					break;
				SpiRamPrintString(5,4+i,((foff+i)*32),0,0);
				if(line == i+4){
					for(u8 k=5; k<26; k++)
						vram[(line*VRAM_TILES_H)+k] += 64;
				}
			}
			if(last_line != line){//have we drawn the title for this file?
				last_line = line;
				if(line > 3 && line < 4+10 && (foff+(line-4)) < total_files){//valid filename area?
					u32 fbase = (u32)((foff+(line-4))*32);
					SpiRamReadInto((u8)(fbase>>16), (u16)(fbase&0xFFFF), as8, 32);
					if(as8[0] != '/'){//not a directory?
						FRESULT res;
						for(u8 i=0; i<3; i++){
							res =	pf_open((char *)as8);
							if(!res){
								pf_lseek(0);
								break;
							}
							WaitVsync(30);
						}
						if(!res){
							WORD br;
							pf_read(as8,20,&br);
							as8[20] = 0;//terminate string
							while(rcv_spi() != 0xFF);//wait for MISO idle
							UMPrint(0,1,PSTR("Title:"));
							UMPrintRam(7,1,(char *)as8);
							u8 slen = 0;
							for(u8 i=0; i<20; i++){
								if(as8[i] == 0)
									break;
								slen++;
							}
							for(u8 i=slen+7; i<20+7; i++)
								SetTile(i, 1, 0);
						}
					}else{
						UMPrint(0,1,PSTR("(Directory)"));
						for(u8 i=11; i<7+20; i++)
							SetTile(i,1,0);//blank over any remnants
					}
				}else
					for(u8 i=0; i<7+20; i++)
						SetTile(i,1,0);//blank any remnants
			}
			if(click){
				if(ButtonHit(4, SCREEN_TILES_V-1, 6, 1)){//cancel
					//SpiRamCopyStringNoBuffer(0, (u32)(detected_ram-64), 64);//restore previously playing filename, if any
					//if(SpiRamReadU8(0,0) != 0)
					//	PlayMOD(1);//force quick reload since lower memory is corrupted by GUI strings...
					PlayMOD(0);//HACK must fix any memory corrupted by file name data...
					goto FILE_SELECT_END;
				}else if(ButtonHit(26,2,1,1)){//up scroll
					if(foff < 10)
						foff = 0;
					else
						foff -= 10;
				}else if(ButtonHit(26,SCREEN_TILES_V-1,1,1)){//down scroll
					if(foff+10 <= total_files)
						foff += 10;
				}else if(ButtonHit(5,4,20,10)){//filename area?
					if(foff+(line-4) < total_files){//valid file?
						SpiRamCopyStringNoBuffer(0, (u32)((foff+(line-4))*32), 32);//copy this entry to first 32 bytes...
						char c = SpiRamReadU8(0, 0);
						if(c == '/'){//not a file, this is a directory to enter
							loaded_dir = 0;
							SpiRamReadInto(0,1,as8,64);//skip '/'
							NextDir((char *)as8);
						}else{//file, try to play it
							if(PlayMOD(0)){//failed?
								SpiRamWriteU8(0,0,0);//erase title string, since now we don't have anything loaded now..
								ClearVram();
								loaded_dir = 0;
							}
						}
						goto FILE_SELECT_END;
					}
				}else if(ButtonHit(5,3,20,1)){//previous directory?
					if(notroot){
						PreviousDir();
						loaded_dir = 0;
					}
				}
			}
		}else{//network load
			break;
		}
	}
FILE_SELECT_END:
	SpiRamCopyStringNoBuffer((u32)(detected_ram-64), 0, 64);//save currently playing file
	play_state &= ~PS_DRAWN;
	PrintSongTitle((CONT_BAR_X/8),(CONT_BAR_Y/8)+2,20);
	if(sprites[0].y > 20)
		sprites[0].y = 18;
	WaitVsync(1);
}