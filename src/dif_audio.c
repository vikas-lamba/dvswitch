// Copyright 2007-2009 Ben Hutchings.
// See the file "COPYING" for licence details.

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "dif.h"

static int16_t decode_12bit(unsigned code)
{
    if (code < 0x200)
    {
	return code;
    }
    else if (code < 0x800)
    {
        unsigned scale = (code >> 8) - 1;
        return ((code & 0xff) + 0x100) << scale;
    }
    else if (code == 0x800)
    {
	return 0;
    }
    else if (code < 0xe00)
    {
	unsigned scale = 14 - (code >> 8);
	return (((int)(code & 0xff) - 0x100) << scale) - 1;
    }
    else
    {
	return (int)code - 0x1000;
    }
}

unsigned dv_buffer_get_audio(const uint8_t * buffer, int16_t * samples)
{
    const struct dv_system * system = dv_buffer_system(buffer);
    const uint8_t * as_pack = buffer + (6 + 3 * 16) * DIF_BLOCK_SIZE + 3;

    if (as_pack[0] != 0x50)
	return 0;

    enum dv_sample_rate sample_rate_code = (as_pack[4] >> 3) & 7;
    if (sample_rate_code >= dv_sample_rate_count)
	return 0;

    unsigned quant = as_pack[4] & 7;
    if (quant > 1)
	return 0;

    unsigned sample_count = 2 * (system->sample_counts[sample_rate_code].min +
				 (as_pack[1] & 0x3f));

    for (unsigned seq = 0;
	 seq != (quant ? system->seq_count / 2 : system->seq_count);
	 ++seq)
    {
	for (unsigned block_n = 0; block_n != 9; ++block_n)
	{
	    const uint8_t * block =
		&buffer[seq * DIF_SEQUENCE_SIZE +
			(6 + 16 * block_n) * DIF_BLOCK_SIZE];

	    if (quant) // 12-bit
	    {
		for (unsigned i = 0; i != 24; ++i)
		{
		    unsigned pos = (system->audio_shuffle[seq][block_n] +
				    i * system->seq_count * 9);
		    if (pos < sample_count)
		    {
			unsigned code = ((block[8 + 3 * i] << 4) +
					 (block[8 + 3 * i + 2] >> 4));
			samples[pos] = decode_12bit(code);
		    }

		    pos = (system->audio_shuffle[
			       seq + system->seq_count / 2][block_n] +
			   i * system->seq_count * 9);
		    if (pos < sample_count)
		    {
			unsigned code = ((block[8 + 3 * i + 1] << 4) +
					  (block[8 + 3 * i + 2] & 0xf));
			samples[pos] = decode_12bit(code);
		    }
		}
	    }
	    else // 16-bit
	    {
		for (unsigned i = 0; i != 36; ++i)
		{
		    unsigned pos = (system->audio_shuffle[seq][block_n] +
				    i * system->seq_count * 9);
		    if (pos < sample_count)
		    {
			int16_t sample = (block[8 + 2 * i] +
					  (block[8 + 2 * i] << 8));
			if (sample == -0x8000)
			    sample = 0;
			samples[pos] = sample;
		    }
		}
	    }
	}
    }

    return sample_count / 2;
}

void dv_buffer_get_audio_levels(const uint8_t * buffer, int * levels)
{
    int16_t samples[2 * 2000];
    unsigned sample_count = dv_buffer_get_audio(buffer, samples);

    assert(2 * sample_count * sizeof(int16_t) <= sizeof(samples));

    // Total of squares of samples, so we can calculate average power.  We shift
    // right to avoid overflow.
    static const unsigned total_shift = 9;
    unsigned total_l = 0, total_r = 0;

    for (unsigned i = 0; i != sample_count; ++i)
    {
	int16_t sample = samples[2 * i];
	total_l += ((unsigned)(sample * sample)) >> total_shift;
	sample = samples[2 * i + 1];
	total_r += ((unsigned)(sample * sample)) >> total_shift;
    }

    // Calculate average power and convert to dB
    levels[0] = (total_l == 0 ? INT_MIN
		 : (int)(log10((double)total_l * (1 << total_shift) /
			       ((double)sample_count * (0x7fff * 0x7fff)))
			 * 10.0));
    levels[1] = (total_r == 0 ? INT_MIN
		 : (int)(log10((double)total_r * (1 << total_shift) /
			       ((double)sample_count * (0x7fff * 0x7fff)))
			 * 10.0));
}

void dv_buffer_dub_audio(uint8_t * dest, const uint8_t * source)
{
    // Copy AAUX blocks.  These are every 16th block in each DIF
    // sequence, starting from block 6.

    const struct dv_system * system = dv_buffer_system(dest);
    assert(dv_buffer_system(source) == system);

    for (unsigned seq_num = 0; seq_num != system->seq_count; ++seq_num)
    {
	for (unsigned block_num = 0; block_num != 9; ++block_num)
	{
	    ptrdiff_t block_pos = (DIF_SEQUENCE_SIZE * seq_num
				   + DIF_BLOCK_SIZE * (6 + block_num * 16));
	    memcpy(dest + block_pos, source + block_pos, DIF_BLOCK_SIZE);
	}
    }
}

void dv_buffer_silence_audio(uint8_t * buffer,
			     enum dv_sample_rate sample_rate_code,
			     unsigned serial_num)
{
    const struct dv_system * system = dv_buffer_system(buffer);
    unsigned sample_count =
	system->sample_counts[sample_rate_code].std_cycle[
	    serial_num % system->sample_counts[sample_rate_code].std_cycle_len];

    // Each audio block has a 3-byte block id, a 5-byte AAUX
    // pack, and 72 bytes of samples.  Audio block 3 in each
    // sequence has an AS (audio source) pack, audio block 4
    // has an ASC (audio source control) pack, and the other
    // packs seem to be optional.
    static const uint8_t aaux_blank_pack[DIF_PACK_SIZE] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    uint8_t aaux_as_pack[DIF_PACK_SIZE] = {
	// pack id; 0x50 for AAUX source
	0x50,
	// bits 0-5: number of samples in frame minus minimum value
	// bit 6: flag "should be 1"
	// bit 7: flag for unlocked audio sampling
	(sample_count - system->sample_counts[sample_rate_code].min)
	| (1 << 6) | (1 << 7),
	// bits 0-3: audio mode
	// bit 4: flag for independent channels
	// bit 5: flag for "lumped" stereo (?)
	// bits 6-7: number of audio channels per block minus 1
	0,
	// bits 0-4: system type; 0x0 for DV
	// bit 5: frame rate; 0 for 29.97 fps, 1 for 25 fps
	// bit 6: flag for multi-language audio
	// bit 7: ?
	dv_buffer_system_code(buffer) << 5,
	// bits 0-2: quantisation; 0 for 16-bit LPCM
	// bits 3-5: sample rate code
	// bit 6: time constant of emphasis; must be 1
	// bit 7: flag for no emphasis
	(sample_rate_code << 3) | (1 << 6) | (1 << 7)
    };
    static const uint8_t aaux_asc_pack[DIF_PACK_SIZE] = {
	// pack id; 0x51 for AAUX source control
	0x51,
	// bits 0-1: emphasis flag and ?
	// bits 2-3: compression level; 0 for once
	// bits 4-5: input type; 1 for digital
	// bits 6-7: copy generation management system; 0 for unrestricted
	(1 << 4),
	// bits 0-2: ?
	// bits 3-5: recording mode; 1 for original (XXX should indicate dub)
	// bit 6: recording end flag, inverted
	// bit 7: recording start flag, inverted
	(1 << 3) | (1 << 6) | (1 << 7),
	// bits 0-6: speed; 0x20 seems to be normal
	// bit 7: direction: 1 for forward
	0x20 | (1 << 7),
	// bits 0-6: genre; 0x7F seems to be unknown
	// bit 7: reserved
	0x7F
    };

    for (unsigned seq_num = 0; seq_num != system->seq_count; ++seq_num)
    {
	for (unsigned block_num = 0; block_num != 9; ++block_num)
	{
	    ptrdiff_t block_pos = (DIF_SEQUENCE_SIZE * seq_num
				   + DIF_BLOCK_SIZE * (6 + block_num * 16));
	    memcpy(buffer + block_pos + DIF_BLOCK_ID_SIZE,
		   block_num == 3 ? aaux_as_pack
		   : block_num == 4 ? aaux_asc_pack
		   : aaux_blank_pack,
		   DIF_PACK_SIZE);
	    memset(buffer + block_pos + DIF_BLOCK_ID_SIZE + DIF_PACK_SIZE,
		   0,
		   DIF_BLOCK_SIZE - DIF_BLOCK_ID_SIZE - DIF_PACK_SIZE);
	}
    }
}
