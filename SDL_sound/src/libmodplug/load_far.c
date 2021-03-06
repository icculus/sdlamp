/*
 * This source code is public domain.
 *
 * Authors: Olivier Lapicque <olivierl@jps.net>
*/

////////////////////////////////////////
// Farandole (FAR) module loader      //
////////////////////////////////////////
#include "libmodplug.h"

#define FARFILEMAGIC	0xFE524146	// "FAR"

#pragma pack(1)

typedef struct FARHEADER1
{
	DWORD id;				// file magic FAR=
	CHAR songname[40];		// songname
	CHAR magic2[3];			// 13,10,26
	WORD headerlen;			// remaining length of header in bytes
	BYTE version;			// 0xD1
	BYTE onoff[16];
	BYTE edit1[9];
	BYTE speed;
	BYTE panning[16];
	BYTE edit2[4];
	WORD stlen;
} FARHEADER1;

typedef struct FARHEADER2
{
	BYTE orders[256];
	BYTE numpat;
	BYTE snglen;
	BYTE loopto;
	WORD patsiz[256];
} FARHEADER2;

typedef struct FARSAMPLE
{
	CHAR samplename[32];
	DWORD length;
	BYTE finetune;
	BYTE volume;
	DWORD reppos;
	DWORD repend;
	BYTE type;
	BYTE loop;
} FARSAMPLE;

#pragma pack()


BOOL CSoundFile_ReadFAR(CSoundFile *_this, const BYTE *lpStream, DWORD dwMemLength)
//---------------------------------------------------------------
{
	const FARHEADER1 *pmh1 = (const FARHEADER1 *)lpStream;
	const FARHEADER2 *pmh2;
	MODINSTRUMENT *pins;
	DWORD dwMemPos = sizeof(FARHEADER1);
	UINT headerlen, stlen, i;
	BYTE samplemap[8];
	WORD *patsiz;

	if ((!lpStream) || (dwMemLength < 1024) || (bswapLE32(pmh1->id) != FARFILEMAGIC)
	 || (pmh1->magic2[0] != 13) || (pmh1->magic2[1] != 10) || (pmh1->magic2[2] != 26)) return FALSE;
	headerlen = bswapLE16(pmh1->headerlen);
	stlen = bswapLE16( pmh1->stlen );
	if ((headerlen >= dwMemLength) || (dwMemPos + stlen + sizeof(FARHEADER2) >= dwMemLength)) return FALSE;
	// Globals
	_this->m_nType = MOD_TYPE_FAR;
	_this->m_nChannels = 16;
	_this->m_nInstruments = 0;
	_this->m_nSamples = 0;
	_this->m_nSongPreAmp = 0x20;
	_this->m_nDefaultSpeed = pmh1->speed;
	_this->m_nDefaultTempo = 80;
	_this->m_nDefaultGlobalVolume = 256;

	// Channel Setting
	for (i=0; i<16; i++)
	{
		_this->ChnSettings[i].dwFlags = 0;
		_this->ChnSettings[i].nPan = ((pmh1->panning[i] & 0x0F) << 4) + 8;
		_this->ChnSettings[i].nVolume = 64;
	}
	// Reading comment
	if (stlen)
	{
		dwMemPos += stlen;
	}
	// Reading orders
	if (sizeof(FARHEADER2) > dwMemLength - dwMemPos) return TRUE;
	pmh2 = (const FARHEADER2 *)(lpStream + dwMemPos);
	dwMemPos += sizeof(FARHEADER2);
	if (dwMemPos >= dwMemLength) return TRUE;
	for (i=0; i<MAX_ORDERS; i++)
	{
		_this->Order[i] = (i <= pmh2->snglen) ? pmh2->orders[i] : 0xFF;
	}
	_this->m_nRestartPos = pmh2->loopto;
	// Reading Patterns	
	dwMemPos += headerlen - (869 + stlen);
	if (dwMemPos >= dwMemLength) return TRUE;

	patsiz = (WORD *)pmh2->patsiz;
	for (i=0; i<256; i++) if (patsiz[i])
	{
		MODCOMMAND *m;
		const BYTE *p;
		UINT len, max, rows, patbrk, patlen;
		patlen = bswapLE16(patsiz[i]);
		if ((i >= MAX_PATTERNS) || (patlen < 2))
		{
			dwMemPos += patlen;
			continue;
		}
		if (dwMemPos + patlen >= dwMemLength) return TRUE;
		max  = (patlen - 2) & ~3;
		rows = (patlen - 2) >> 6;
		if (!rows)
		{
			dwMemPos += patlen;
			continue;
		}
		if (rows > 256) rows = 256;
		if (rows < 16) rows = 16;
		if (max > rows*16*4) max = rows*16*4;
		_this->PatternSize[i] = rows;
		if ((_this->Patterns[i] = CSoundFile_AllocatePattern(rows, _this->m_nChannels)) == NULL) return TRUE;
		m = _this->Patterns[i];
		patbrk = lpStream[dwMemPos];
		p = lpStream + dwMemPos + 2;
		for (len=0; len<max; len += 4, m++)
		{
			BYTE note = p[len];
			BYTE ins = p[len+1];
			BYTE vol = p[len+2];
			BYTE eff = p[len+3];
			if (note)
			{
				m->instr = ins + 1;
				m->note = note + 36;
			}
			if (vol >= 0x01 && vol <= 0x10)
			{
				m->volcmd = VOLCMD_VOLUME;
				m->vol = (vol - 1) << 2;
			}
			switch(eff & 0xF0)
			{
			// 1.x: Portamento Up
			case 0x10:
				m->command = CMD_PORTAMENTOUP;
				m->param = eff & 0x0F;
				break;
			// 2.x: Portamento Down
			case 0x20:
				m->command = CMD_PORTAMENTODOWN;
				m->param = eff & 0x0F;
				break;
			// 3.x: Tone-Portamento
			case 0x30:
				m->command = CMD_TONEPORTAMENTO;
				m->param = (eff & 0x0F) << 2;
				break;
			// 4.x: Retrigger
			case 0x40:
				m->command = CMD_RETRIG;
				m->param = 6 / (1+(eff&0x0F)) + 1;
				break;
			// 5.x: Set Vibrato Depth
			case 0x50:
				m->command = CMD_VIBRATO;
				m->param = (eff & 0x0F);
				break;
			// 6.x: Set Vibrato Speed
			case 0x60:
				m->command = CMD_VIBRATO;
				m->param = (eff & 0x0F) << 4;
				break;
			// 7.x: Vol Slide Up
			case 0x70:
				m->command = CMD_VOLUMESLIDE;
				m->param = (eff & 0x0F) << 4;
				break;
			// 8.x: Vol Slide Down
			case 0x80:
				m->command = CMD_VOLUMESLIDE;
				m->param = (eff & 0x0F);
				break;
			// A.x: Port to vol
			case 0xA0:
				m->volcmd = VOLCMD_VOLUME;
				m->vol = ((eff & 0x0F) << 2) + 4;
				break;
			// B.x: Set Balance
			case 0xB0:
				m->command = CMD_PANNING8;
				m->param = (eff & 0x0F) << 4;
				break;
			// F.x: Set Speed
			case 0xF0:
				m->command = CMD_SPEED;
				m->param = eff & 0x0F;
				break;
			default:
				if ((patbrk) &&	(patbrk+1 == (len >> 6)) && (patbrk+1 != rows-1))
				{
					m->command = CMD_PATTERNBREAK;
					patbrk = 0;
				}
			}
		}
		dwMemPos += patlen;
	}
	// Reading samples
	if (dwMemPos + 8 >= dwMemLength) return TRUE;
	SDL_memcpy(samplemap, lpStream+dwMemPos, 8);
	dwMemPos += 8;
	pins = &_this->Ins[1];
	for (i=0; i<64; i++, pins++) if (samplemap[i >> 3] & (1 << (i & 7)))
	{
		const FARSAMPLE *pfs;
		DWORD length;
		if (dwMemPos + sizeof(FARSAMPLE) > dwMemLength) return TRUE;
		pfs = (const FARSAMPLE*)(lpStream + dwMemPos);
		dwMemPos += sizeof(FARSAMPLE);
		_this->m_nSamples = i + 1;
		length = bswapLE32(pfs->length); /* endian fix - Toad */
		pins->nLength = length;
		pins->nLoopStart = bswapLE32(pfs->reppos);
		pins->nLoopEnd = bswapLE32(pfs->repend);
		pins->nFineTune = 0;
		pins->nC4Speed = 8363*2;
		pins->nGlobalVol = 64;
		pins->nVolume = pfs->volume << 4;
		pins->uFlags = 0;
		if ((pins->nLength > 3) && (dwMemPos + 4 < dwMemLength))
		{
			if (pfs->type & 1)
			{
				pins->uFlags |= CHN_16BIT;
				pins->nLength >>= 1;
				pins->nLoopStart >>= 1;
				pins->nLoopEnd >>= 1;
			}
			if ((pfs->loop & 8) && (pins->nLoopEnd > 4)) pins->uFlags |= CHN_LOOP;
			CSoundFile_ReadSample(_this, pins, (pins->uFlags & CHN_16BIT) ? RS_PCM16S : RS_PCM8S,
						(LPSTR)(lpStream+dwMemPos), dwMemLength - dwMemPos);
		}
		dwMemPos += length;
	}
	return TRUE;
}
