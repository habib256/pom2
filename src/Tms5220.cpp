// POM2 — adapted from MAME, BSD-3-Clause.
// Copyright Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu, Raphael Nabet,
// Couriersud, Michael Zapf. See THIRD-PARTY.md and Tms5220Tables.h.
// MAME src/devices/sound/tms5220.cpp:960-1370,1561-1662 (2026-10-18).
// TMS5220C: native clock/80 synthesis, patent interpolation/lattice/DAC.
#include "Tms5220.h"
#include "Tms5220Tables.h"
#include "ByteIO.h"
#include <algorithm>
#include <iterator>
#define LOGMASKED(...) ((void)0)
#define INTERP_SHIFT >> m_coeff->interp_coeff[s_.m_IP]
#define TMS5220_HAS_RATE_CONTROL 1
namespace pom2 {
namespace {
constexpr uint8_t reload_table[4] = {0, 2, 4, 6};
constexpr const auto* m_coeff = &tmsdetail::tms5220_coeff;
}
void Tms5220::reset()
{
    s_ = {};
    s_.m_buffer_empty = s_.m_buffer_low = true;
    s_.m_OLDE = s_.m_OLDP = s_.m_inhibit = true;
    s_.m_subc_reload = 1;
    s_.m_RNG = 0x1FFF;
    s_.m_command_register = 0xFF;
}
void Tms5220::update_fifo_status_and_ints()
{
    const bool low = s_.m_fifo_count <= 8, empty = s_.m_fifo_count == 0;
    if ((low && !s_.m_buffer_low) || (empty && !s_.m_buffer_empty)) s_.m_irq_pin = true;
    s_.m_buffer_low = low; s_.m_buffer_empty = empty;
    if (empty && s_.m_DDIS) s_.m_TALK = s_.m_SPEN = false;
    if (s_.m_previous_talk_status && !talk_status()) {
        s_.m_irq_pin = true;
        s_.m_DDIS = false;
        if ((s_.m_command_register & 0x70) == 0x10) {   // a held Read Byte
            s_.m_command_register = 0xFF;
            s_.m_read_byte_register = 0xFF; s_.m_RDB_flag = true;
        }
    }
    s_.m_previous_talk_status = talk_status();
}
uint8_t Tms5220::status()
{
    if (s_.m_RDB_flag) { s_.m_RDB_flag = false; return s_.m_read_byte_register; }
    s_.m_irq_pin = false;
    return uint8_t((talk_status() ? 0x80 : 0) | (s_.m_buffer_low ? 0x40 : 0) | (s_.m_buffer_empty ? 0x20 : 0));
}
int Tms5220::read_bits(int count)
{
    if (!s_.m_DDIS) return (1 << count) - 1; // Echo+ has no speech ROM.
    int value = 0;
    while (count--) {
        value <<= 1;
        if (!s_.m_fifo_count) continue;
        value |= (s_.m_fifo[s_.m_fifo_head] >> s_.m_fifo_bits_taken) & 1;
        if (++s_.m_fifo_bits_taken == 8) {
            s_.m_fifo[s_.m_fifo_head] = 0;
            s_.m_fifo_head = (s_.m_fifo_head + 1) & 15;
            --s_.m_fifo_count; s_.m_fifo_bits_taken = 0;
            update_fifo_status_and_ints();
        }
    }
    return value;
}
// zpar/uv_zpar + OLDE/OLDP and the "new frame" indices of silence: what both
// speech starts (SPEAK at :1430-1448, the FIFO's SPEN edge at :688-712) set.
void Tms5220::zeroNewFrame()
{
    s_.m_zpar = s_.m_uv_zpar = s_.m_OLDE = s_.m_OLDP = true;
    s_.m_new_frame_energy_idx = s_.m_new_frame_pitch_idx = 0;
    for (int i = 0; i < 10; ++i) s_.m_new_frame_k_idx[i] = i < 4 ? 0 : i < 7 ? 15 : 7;
}

void Tms5220::write(uint8_t value)
{
    // MAME tms5220.cpp:672-730,1393-1546: SPEAK EXTERNAL's ninth byte
    // starts speech. In external mode even $70 is data, not RESET.
    if (s_.m_DDIS) {
        if (s_.m_fifo_count == 16) return;
        const bool wasLow = s_.m_buffer_low;
        s_.m_fifo[s_.m_fifo_tail] = value;
        s_.m_fifo_tail = (s_.m_fifo_tail + 1) & 15;
        ++s_.m_fifo_count;
        update_fifo_status_and_ints();
        // MAME :688-712 — the edge that raises SPEN also zeroes the frame
        // it will interpolate from. A late refill used to resume from the
        // half-parsed frame of the phrase that ran dry.
        if (!s_.m_SPEN && wasLow && !s_.m_buffer_low) {
            zeroNewFrame();
            s_.m_SPEN = true;
        }
        return;
    }
    switch (value & 0x70) {
    case 0x00: case 0x20: s_.m_c_variant_rate = value & 15; break;
    case 0x10:
        // Read Byte while talking is HELD in the command register and run when
        // talk status falls (MAME tms5220.cpp:1395-1415, :794-807). Executed at
        // once, it made the next status read return $FF mid-utterance.
        if (talk_status()) { s_.m_command_register = value; break; }
        s_.m_read_byte_register = 0xFF; s_.m_RDB_flag = true; break;
    case 0x30: case 0x40: break; // No VSM fitted on Echo+.
    case 0x50: case 0x60:
        s_.m_DDIS = (value & 0x70) == 0x60;
        s_.m_SPEN = !s_.m_DDIS;
        if (s_.m_DDIS) {
            std::fill(std::begin(s_.m_fifo), std::end(s_.m_fifo), 0);
            s_.m_fifo_head = s_.m_fifo_tail = s_.m_fifo_count = s_.m_fifo_bits_taken = 0;
        }
        zeroNewFrame();
        s_.m_RDB_flag = false;
        break;
    case 0x70: reset(); break;
    }
    update_fifo_status_and_ints();
}
void Tms5220::process(int16_t *buffer, unsigned int size)
{
	int buf_count = 0;
	int i, bitout;
	int32_t this_sample;

	LOGMASKED(LOG_GENERATION, "process called with size of %d; IP=%d, PC=%d, subcycle=%d, s_.m_SPEN=%d, s_.m_TALK=%d, s_.m_TALKD=%d\n", size, s_.m_IP, s_.m_PC, s_.m_subcycle, s_.m_SPEN, s_.m_TALK, s_.m_TALKD);

	/* loop until the buffer is full or we've stopped speaking */
	while (size > 0)
	{
		if(s_.m_TALKD) // speaking
		{
			/* if we're ready for a new frame to be applied, i.e. when IP=0, PC=12, Sub=1
			* (In reality, the frame was really loaded incrementally during the entire IP=0
			* PC=x time period, but it doesn't affect anything until IP=0 PC=12 happens)
			*/
			if ((s_.m_IP == 0) && (s_.m_PC == 12) && (s_.m_subcycle == 1))
			{
				// HACK for regression testing, be sure to comment out before release!
				//s_.m_RNG = 0x1234;
				// end HACK

				/* appropriately override the interp count if needed; this will be incremented after the frame parse! */
				s_.m_IP = reload_table[s_.m_c_variant_rate&0x3];


				/* Parse a new frame into the new_target_energy, new_target_pitch and new_target_k[] */
				parse_frame();

				/* if the new frame is a stop frame, unset both TALK and SPEN (via TCON). TALKD remains active while the energy is ramping to 0. */
				if (new_frame_stop_flag())
				{
					s_.m_TALK = s_.m_SPEN = false;
					update_fifo_status_and_ints(); // probably not necessary...
				}

				/* in all cases where interpolation would be inhibited, set the inhibit flag; otherwise clear it.
				 * Interpolation inhibit cases:
				 * Old frame was voiced, new is unvoiced
				 * Old frame was silence/zero energy, new has non-zero energy
				 * Old frame was unvoiced, new is voiced
				 * Old frame was unvoiced, new frame is silence/zero energy (non-existent on tms51xx rev D and F (present and working on tms52xx, present but buggy on tms51xx rev A and B))
				 */
				if ( (!old_frame_unvoiced_flag() && new_frame_unvoiced_flag())
					|| (old_frame_unvoiced_flag() && !new_frame_unvoiced_flag())
					|| (old_frame_silence_flag() && !new_frame_silence_flag())
					//|| (s_.m_inhibit && old_frame_unvoiced_flag() && new_frame_silence_flag()) ) //TMS51xx INTERP BUG1
					|| (old_frame_unvoiced_flag() && new_frame_silence_flag()) )
					s_.m_inhibit = true;
				else // normal frame, normal interpolation
					s_.m_inhibit = false;

				/* Debug info for current parsed frame */
				LOGMASKED(LOG_GENERATION, "OLDE: %d; NEWE: %d; OLDP: %d; NEWP: %d\n", old_frame_silence_flag(), new_frame_silence_flag(), old_frame_unvoiced_flag(), new_frame_unvoiced_flag());
				LOGMASKED(LOG_GENERATION, "Processing new frame: %s\n", !s_.m_inhibit ? "Normal Frame" : "Interpolation Inhibited");
				LOGMASKED(LOG_GENERATION, "*** current Energy, Pitch and Ks =      %04d,   %04d, %04d, %04d, %04d, %04d, %04d, %04d, %04d, %04d, %04d, %04d\n",s_.m_current_energy, s_.m_current_pitch, s_.m_current_k[0], s_.m_current_k[1], s_.m_current_k[2], s_.m_current_k[3], s_.m_current_k[4], s_.m_current_k[5], s_.m_current_k[6], s_.m_current_k[7], s_.m_current_k[8], s_.m_current_k[9]);
				LOGMASKED(LOG_GENERATION, "*** target Energy(idx), Pitch, and Ks = %04d(%x),%04d, %04d, %04d, %04d, %04d, %04d, %04d, %04d, %04d, %04d, %04d\n",
					(m_coeff->energytable[s_.m_new_frame_energy_idx] * (1-s_.m_zpar)),
					s_.m_new_frame_energy_idx,
					(m_coeff->pitchtable[s_.m_new_frame_pitch_idx] * (1-s_.m_zpar)),
					(m_coeff->ktable[0][s_.m_new_frame_k_idx[0]] * (1-s_.m_zpar)),
					(m_coeff->ktable[1][s_.m_new_frame_k_idx[1]] * (1-s_.m_zpar)),
					(m_coeff->ktable[2][s_.m_new_frame_k_idx[2]] * (1-s_.m_zpar)),
					(m_coeff->ktable[3][s_.m_new_frame_k_idx[3]] * (1-s_.m_zpar)),
					(m_coeff->ktable[4][s_.m_new_frame_k_idx[4]] * (1-s_.m_uv_zpar)),
					(m_coeff->ktable[5][s_.m_new_frame_k_idx[5]] * (1-s_.m_uv_zpar)),
					(m_coeff->ktable[6][s_.m_new_frame_k_idx[6]] * (1-s_.m_uv_zpar)),
					(m_coeff->ktable[7][s_.m_new_frame_k_idx[7]] * (1-s_.m_uv_zpar)),
					(m_coeff->ktable[8][s_.m_new_frame_k_idx[8]] * (1-s_.m_uv_zpar)),
					(m_coeff->ktable[9][s_.m_new_frame_k_idx[9]] * (1-s_.m_uv_zpar)) );
			}
			else // Not a new frame, just interpolate the existing frame.
			{
				bool inhibit_state = (s_.m_inhibit && (s_.m_IP != 0)); // disable inhibit when reaching the last interp period, but don't overwrite the s_.m_inhibit value
				//Updates to parameters only happen on subcycle '2' (B cycle) of PCs.
				if (s_.m_subcycle == 2)
				{
					switch(s_.m_PC)
					{
						case 0: /* PC = 0, B cycle, write updated energy */
						if (s_.m_IP==0) s_.m_pitch_zero = 0; // this reset happens around the second subcycle during IP=0
						s_.m_current_energy = (s_.m_current_energy + (((m_coeff->energytable[s_.m_new_frame_energy_idx] - s_.m_current_energy)*(1-inhibit_state)) INTERP_SHIFT))*(1-s_.m_zpar);
						break;
						case 1: /* PC = 1, B cycle, write updated pitch */
						s_.m_current_pitch = (s_.m_current_pitch + (((m_coeff->pitchtable[s_.m_new_frame_pitch_idx] - s_.m_current_pitch)*(1-inhibit_state)) INTERP_SHIFT))*(1-s_.m_zpar);
						break;
						case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
						/* PC = 2 through 11, B cycle, write updated K1 through K10 */
						s_.m_current_k[s_.m_PC-2] = (s_.m_current_k[s_.m_PC-2] + (((m_coeff->ktable[s_.m_PC-2][s_.m_new_frame_k_idx[s_.m_PC-2]] - s_.m_current_k[s_.m_PC-2])*(1-inhibit_state)) INTERP_SHIFT))*(1-(((s_.m_PC-2)<4)?s_.m_zpar:s_.m_uv_zpar));
						break;
						case 12: /* PC = 12 */
						/* we should NEVER reach this point, PC=12 doesn't have a subcycle 2 */
						break;
					}
				}
			}

			// calculate the output
			if (old_frame_unvoiced_flag())
			{
				// generate unvoiced samples here
				if (s_.m_RNG & 1)
					s_.m_excitation_data = ~0x3F; /* according to the patent it is (either + or -) half of the maximum value in the chirp table, so either 01000000(0x40) or 11000000(0xC0)*/
				else
					s_.m_excitation_data = 0x40;
			}
			else /* (!old_frame_unvoiced_flag()) */
			{
				// generate voiced samples here
				/* US patent 4331836 Figure 14B shows, and logic would hold, that a pitch based chirp
				 * function has a chirp/peak and then a long chain of zeroes.
				 * The last entry of the chirp rom is at address 0b110011 (51d), the 52nd sample,
				 * and if the address reaches that point the ADDRESS incrementer is
				 * disabled, forcing all samples beyond 51d to be == 51d
				 */
				if (s_.m_pitch_count >= 51)
					s_.m_excitation_data = (int8_t)m_coeff->chirptable[51];
				else /*s_.m_pitch_count < 51*/
					s_.m_excitation_data = (int8_t)m_coeff->chirptable[s_.m_pitch_count];
			}

			// Update LFSR *20* times every sample (once per T cycle), like patent shows
			for (i=0; i<20; i++)
			{
				bitout = ((s_.m_RNG >> 12) & 1) ^
						((s_.m_RNG >>  3) & 1) ^
						((s_.m_RNG >>  2) & 1) ^
						((s_.m_RNG >>  0) & 1);
				s_.m_RNG <<= 1;
				s_.m_RNG |= bitout;
			}
			this_sample = lattice_filter(); /* execute lattice filter */

			//LOGMASKED(LOG_GENERATION_VERBOSE, "C:%01d; ",s_.m_subcycle);
			LOGMASKED(LOG_GENERATION_VERBOSE, "IP:%01d PC:%02d X:%04d E:%03d P:%03d Pc:%03d ",s_.m_IP, s_.m_PC, s_.m_excitation_data, s_.m_current_energy, s_.m_current_pitch, s_.m_pitch_count);
			//LOGMASKED(LOG_GENERATION_VERBOSE, "X:%04d E:%03d P:%03d Pc:%03d ", s_.m_excitation_data, s_.m_current_energy, s_.m_current_pitch, s_.m_pitch_count);
			for (i=0; i<10; i++)
				LOGMASKED(LOG_GENERATION_VERBOSE, "K%d:%04d ", i+1, s_.m_current_k[i]);
			LOGMASKED(LOG_GENERATION_VERBOSE, "Out:%06d ", this_sample);
//#ifdef TMS5220_PERFECT_INTERPOLATION_HACK
//          LOGMASKED(LOG_GENERATION_VERBOSE, "%d%d%d%d",s_.m_old_zpar,s_.m_zpar,s_.m_old_uv_zpar,s_.m_uv_zpar);
//#else
//          LOGMASKED(LOG_GENERATION_VERBOSE, "x%dx%d",s_.m_zpar,s_.m_uv_zpar);
//#endif
			LOGMASKED(LOG_GENERATION_VERBOSE, "\n");

			/* next, force result to 14 bits (since its possible that the addition at the final (k1) stage of the lattice overflowed) */
			while (this_sample > 16383) this_sample -= 32768;
			while (this_sample < -16384) this_sample += 32768;
			if (false == 0) // analog SPK pin output is only 8 bits, with clipping
				buffer[buf_count] = clip_analog(this_sample);
			else // digital I/O pin output is 12 bits
			{
				this_sample &= ~0xF;
				// input:  ssss ssss ssss ssss ssnn nnnn nnnn 0000
				// N taps:                       ^^ ^^^            = 0x3E00;
				// output: ssss ssss ssss ssss snnn nnnn nnnN NNNN
				buffer[buf_count] = (this_sample * 2)|((this_sample&0x3E00)>>9);
			}
			// Update all counts

			s_.m_subcycle++;
			if ((s_.m_subcycle == 2) && (s_.m_PC == 12)) // RESETF3
			{
				/* Circuit 412 in the patent acts a reset, resetting the pitch counter to 0
				 * if INHIBIT was true during the most recent frame transition.
				 * The exact time this occurs is betwen IP=7, PC=12 sub=0, T=t12
				 * and s_.m_IP = 0, PC=0 sub=0, T=t12, a period of exactly 20 cycles,
				 * which overlaps the time OLDE and OLDP are updated at IP=7 PC=12 T17
				 * (and hence INHIBIT itself 2 t-cycles later).
				 * According to testing the pitch zeroing lasts approximately 2 samples.
				 * We set the zeroing latch here, and unset it on PC=1 in the generator.
				 */
				if ((s_.m_IP == 7) && s_.m_inhibit) s_.m_pitch_zero = true;
				if (s_.m_IP == 7) // RESETL4
				{
					// Latch OLDE and OLDP
					//if (old_frame_silence_flag()) s_.m_uv_zpar = false; // TMS51xx INTERP BUG2
					s_.m_OLDE = new_frame_silence_flag(); // old_frame_silence_flag()
					s_.m_OLDP = new_frame_unvoiced_flag(); // old_frame_unvoiced_flag()
					/* if TALK was clear last frame, halt speech now, since TALKD (latched from TALK on new frame) just went inactive. */

					LOGMASKED(LOG_GENERATION, "RESETL4, about to update status: IP=%d, PC=%d, subcycle=%d, s_.m_SPEN=%d, s_.m_TALK=%d, s_.m_TALKD=%d\n", s_.m_IP, s_.m_PC, s_.m_subcycle, s_.m_SPEN, s_.m_TALK, s_.m_TALKD);
					if ((!s_.m_TALK) && (!s_.m_SPEN))
						LOGMASKED(LOG_GENERATION, "tms5220_process: processing frame: TALKD = 0 caused by stop frame or buffer empty, halting speech.\n");

					s_.m_TALKD = s_.m_TALK; // TALKD is latched from TALK
					update_fifo_status_and_ints(); // to trigger an interrupt if talk_status has changed
					if ((!s_.m_TALK) && s_.m_SPEN) s_.m_TALK = true; // TALK is only activated if it wasn't already active, if s_.m_SPEN is active, and if we're in RESETL4 (which we are).

					LOGMASKED(LOG_GENERATION, "RESETL4, status updated: IP=%d, PC=%d, subcycle=%d, s_.m_SPEN=%d, s_.m_TALK=%d, s_.m_TALKD=%d\n", s_.m_IP, s_.m_PC, s_.m_subcycle, s_.m_SPEN, s_.m_TALK, s_.m_TALKD);
				}
				s_.m_subcycle = s_.m_subc_reload;
				s_.m_PC = 0;
				s_.m_IP++;
				s_.m_IP &= 0x7;
			}
			else if (s_.m_subcycle == 3)
			{
				s_.m_subcycle = s_.m_subc_reload;
				s_.m_PC++;
			}
			s_.m_pitch_count++;
			if ((s_.m_pitch_count >= s_.m_current_pitch) || s_.m_pitch_zero) s_.m_pitch_count = 0;
			s_.m_pitch_count &= 0x1FF;
		}
		else // s_.m_TALKD == 0
		{
			s_.m_subcycle++;
			if ((s_.m_subcycle == 2) && (s_.m_PC == 12)) // RESETF3
			{
				if (s_.m_IP == 7) // RESETL4
				{
					s_.m_TALKD = s_.m_TALK; // TALKD is latched from TALK
					update_fifo_status_and_ints(); // probably not necessary
					if ((!s_.m_TALK) && s_.m_SPEN) s_.m_TALK = true; // TALK is only activated if it wasn't already active, if s_.m_SPEN is active, and if we're in RESETL4 (which we are).
				}
				s_.m_subcycle = s_.m_subc_reload;
				s_.m_PC = 0;
				s_.m_IP++;
				s_.m_IP&=0x7;
			}
			else if (s_.m_subcycle == 3)
			{
				s_.m_subcycle = s_.m_subc_reload;
				s_.m_PC++;
			}
			buffer[buf_count] = -1; /* should be just -1; actual chip outputs -1 every idle sample; (cf note in data sheet, p 10, table 4) */
		}
	buf_count++;
	size--;
	}
}

int16_t Tms5220::clip_analog(int16_t cliptemp) const
{
	/* clipping, just like the patent shows:
	 * the top 10 bits of this result are visible on the digital output IO pin.
	 * next, if the top 3 bits of the 14 bit result are all the same, the
	 * lowest of those 3 bits plus the next 7 bits are the signed analog
	 * output, otherwise the low bits are all forced to match the inverse of
	 * the topmost bit, i.e.:
	 * 1x xxxx xxxx xxxx -> 0b10000000
	 * 11 1bcd efgh xxxx -> 0b1bcdefgh
	 * 00 0bcd efgh xxxx -> 0b0bcdefgh
	 * 0x xxxx xxxx xxxx -> 0b01111111
	 */
	if ((cliptemp > 2047) || (cliptemp < -2048))
		LOGMASKED(LOG_CLIP, "clipping cliptemp to range; was %d\n", cliptemp);
	if (cliptemp > 2047) cliptemp = 2047;
	else if (cliptemp < -2048) cliptemp = -2048;
	/* at this point the analog output is tapped */
	cliptemp &= ~0xF;
	// input:  ssss snnn nnnn 0000
	// N taps:       ^^^ ^^^^      = 0x07F0
	// P taps:       ^             = 0x0400
	// output: snnn nnnn NNNN NNNP
	return (cliptemp * 16)|((cliptemp&0x7F0)>>3)|((cliptemp&0x400)>>10); // upshift and range adjust
}

int32_t Tms5220::matrix_multiply(int32_t a, int32_t b) const
{
	int32_t result;
	while (a>511) { a-=1024; }
	while (a<-512) { a+=1024; }
	while (b>16383) { b-=32768; }
	while (b<-16384) { b+=32768; }
	result = ((a*b)>>9); /** TODO: this isn't technically right to the chip, which truncates the lowest result bit, but it causes glitches otherwise. **/
	if (result>16383) LOGMASKED(LOG_MULTIPLY, "matrix multiplier overflowed! a: %x, b: %x, result: %x", a, b, result);
	if (result<-16384) LOGMASKED(LOG_MULTIPLY, "matrix multiplier underflowed! a: %x, b: %x, result: %x", a, b, result);
	return result;
}

int32_t Tms5220::lattice_filter()
{
	// Lattice filter here
	// Aug/05/07: redone as unrolled loop, for clarity - LN
	/* Originally Copied verbatim from table I in US patent 4,209,804, now
	  updated to be in same order as the actual chip does it, not that it matters.

	  notation equivalencies from table:
	  Yn(i) == s_.m_u[n-1]
	  Kn = s_.m_current_k[n-1]
	  bn = s_.m_x[n-1]
	 */
		s_.m_u[10] = matrix_multiply(s_.m_previous_energy, (s_.m_excitation_data * 64));  //Y(11)
		s_.m_u[9] = s_.m_u[10] - matrix_multiply(s_.m_current_k[9], s_.m_x[9]);
		s_.m_u[8] = s_.m_u[9] - matrix_multiply(s_.m_current_k[8], s_.m_x[8]);
		s_.m_u[7] = s_.m_u[8] - matrix_multiply(s_.m_current_k[7], s_.m_x[7]);
		s_.m_u[6] = s_.m_u[7] - matrix_multiply(s_.m_current_k[6], s_.m_x[6]);
		s_.m_u[5] = s_.m_u[6] - matrix_multiply(s_.m_current_k[5], s_.m_x[5]);
		s_.m_u[4] = s_.m_u[5] - matrix_multiply(s_.m_current_k[4], s_.m_x[4]);
		s_.m_u[3] = s_.m_u[4] - matrix_multiply(s_.m_current_k[3], s_.m_x[3]);
		s_.m_u[2] = s_.m_u[3] - matrix_multiply(s_.m_current_k[2], s_.m_x[2]);
		s_.m_u[1] = s_.m_u[2] - matrix_multiply(s_.m_current_k[1], s_.m_x[1]);
		s_.m_u[0] = s_.m_u[1] - matrix_multiply(s_.m_current_k[0], s_.m_x[0]);
		s_.m_x[9] = s_.m_x[8] + matrix_multiply(s_.m_current_k[8], s_.m_u[8]);
		s_.m_x[8] = s_.m_x[7] + matrix_multiply(s_.m_current_k[7], s_.m_u[7]);
		s_.m_x[7] = s_.m_x[6] + matrix_multiply(s_.m_current_k[6], s_.m_u[6]);
		s_.m_x[6] = s_.m_x[5] + matrix_multiply(s_.m_current_k[5], s_.m_u[5]);
		s_.m_x[5] = s_.m_x[4] + matrix_multiply(s_.m_current_k[4], s_.m_u[4]);
		s_.m_x[4] = s_.m_x[3] + matrix_multiply(s_.m_current_k[3], s_.m_u[3]);
		s_.m_x[3] = s_.m_x[2] + matrix_multiply(s_.m_current_k[2], s_.m_u[2]);
		s_.m_x[2] = s_.m_x[1] + matrix_multiply(s_.m_current_k[1], s_.m_u[1]);
		s_.m_x[1] = s_.m_x[0] + matrix_multiply(s_.m_current_k[0], s_.m_u[0]);
		s_.m_x[0] = s_.m_u[0];
		s_.m_previous_energy = s_.m_current_energy;

		LOGMASKED(LOG_LATTICE, "V:%04d ", s_.m_u[10]);
		for (int i = 9; i >= 0; i--)
		{
			LOGMASKED(LOG_LATTICE, "Y%d:%04d ", i+1, s_.m_u[i]);
		}
		LOGMASKED(LOG_LATTICE, "\n");
		LOGMASKED(LOG_LATTICE, "E:%04d ", err);
		for (int i = 9; i >= 0; i--)
		{
			LOGMASKED(LOG_LATTICE, "b%d:%04d ", i+1, s_.m_x[i]);
		}
		LOGMASKED(LOG_LATTICE, "\n");

		return s_.m_u[0];
}

void Tms5220::parse_frame()
{
	int i, rep_flag;
	/* Since we're parsing a frame, we must be talking, so clear zpar here.
	Also, before we started parsing a frame, the P=0 and E=0 latches were both
	reset by RESETL4, so clear s_.m_uv_zpar here.
	*/
	s_.m_uv_zpar = s_.m_zpar = 0;

	/* We actually don't care how many bits are left in the FIFO here;
	the frame subpart will be processed normally, and any bits extracted
	'past the end' of the FIFO will be read as zeroes; the FIFO being emptied
	will set the /BE latch which will halt speech exactly as if a stop frame
	had been encountered (instead of whatever partial frame was read).
	The same exact circuitry is used for both functions on the real chip, see
	us patent 4335277 sheet 16, gates 232a (decode stop frame) and 232b
	(decode /BE plus DDIS (decode disable) which is active during speak external).
	*/

	/* if the chip is a tms5220C, and the rate mode is set to that each frame (0x04 bit set)
	has a 2 bit rate preceding it, grab two bits here and store them as the rate; */
	if ((TMS5220_HAS_RATE_CONTROL) && (s_.m_c_variant_rate & 0x04))
	{
		i = read_bits(2);
		printbits(i, 2);
		LOGMASKED(LOG_PARSE_FRAME_DUMP_BIN | LOG_PARSE_FRAME_DUMP_HEX, " ");
		s_.m_IP = reload_table[i];
	}
	else // non-5220C and 5220C in fixed rate mode
	s_.m_IP = reload_table[s_.m_c_variant_rate&0x3];

	update_fifo_status_and_ints();
	if (s_.m_DDIS && s_.m_buffer_empty) goto ranout;

	// attempt to extract the energy index
	s_.m_new_frame_energy_idx = read_bits(m_coeff->energy_bits);
	printbits(s_.m_new_frame_energy_idx, m_coeff->energy_bits);
	LOGMASKED(LOG_PARSE_FRAME_DUMP_BIN | LOG_PARSE_FRAME_DUMP_HEX, " ");
	update_fifo_status_and_ints();
	if (s_.m_DDIS && s_.m_buffer_empty) goto ranout;
	// if the energy index is 0 or 15, we're done
	if ((s_.m_new_frame_energy_idx == 0) || (s_.m_new_frame_energy_idx == 15))
		return;


	// attempt to extract the repeat flag
	rep_flag = read_bits(1);
	printbits(rep_flag, 1);
	LOGMASKED(LOG_PARSE_FRAME_DUMP_BIN | LOG_PARSE_FRAME_DUMP_HEX, " ");

	// attempt to extract the pitch
	s_.m_new_frame_pitch_idx = read_bits(m_coeff->pitch_bits);
	printbits(s_.m_new_frame_pitch_idx, m_coeff->pitch_bits);
	LOGMASKED(LOG_PARSE_FRAME_DUMP_BIN | LOG_PARSE_FRAME_DUMP_HEX, " ");
	// if the new frame is unvoiced, be sure to zero out the k5-k10 parameters
	s_.m_uv_zpar = new_frame_unvoiced_flag();
	update_fifo_status_and_ints();
	if (s_.m_DDIS && s_.m_buffer_empty) goto ranout;
	// if this is a repeat frame, just do nothing, it will reuse the old coefficients
	if (rep_flag)
		return;

	// extract first 4 K coefficients
	for (i = 0; i < 4; i++)
	{
		s_.m_new_frame_k_idx[i] = read_bits(m_coeff->kbits[i]);
		printbits(s_.m_new_frame_k_idx[i], m_coeff->kbits[i]);
		LOGMASKED(LOG_PARSE_FRAME_DUMP_BIN | LOG_PARSE_FRAME_DUMP_HEX, " ");
		update_fifo_status_and_ints();
		if (s_.m_DDIS && s_.m_buffer_empty) goto ranout;
	}

	// if the pitch index was zero, we only need 4 K's...
	if (s_.m_new_frame_pitch_idx == 0)
	{
		/* and the rest of the coefficients are zeroed, but that's done in the generator code */
		return;
	}

	// If we got here, we need the remaining 6 K's
	for (i = 4; i < m_coeff->num_k; i++)
	{
		s_.m_new_frame_k_idx[i] = read_bits(m_coeff->kbits[i]);
		printbits(s_.m_new_frame_k_idx[i], m_coeff->kbits[i]);
		LOGMASKED(LOG_PARSE_FRAME_DUMP_BIN | LOG_PARSE_FRAME_DUMP_HEX, " ");
		update_fifo_status_and_ints();
		if (s_.m_DDIS && s_.m_buffer_empty) goto ranout;
	}
	LOGMASKED(LOG_PARSE_FRAME_DUMP_BIN | LOG_PARSE_FRAME_DUMP_HEX, "\n");

	if (s_.m_DDIS)
		LOGMASKED(LOG_PARSE, "Parsed a frame successfully in FIFO - %d bits remaining\n", (s_.m_fifo_count*8)-(s_.m_fifo_bits_taken));
	else
		LOGMASKED(LOG_PARSE, "Parsed a frame successfully in ROM\n");
	return;

	ranout:
	LOGMASKED(LOG_FRAME_ERRORS, "Ran out of bits on a parse!\n");
	return;
}
// Endian-independent checkpoint of every FIFO, interpolation and filter latch.
void Tms5220::appendSnapshot(std::vector<uint8_t>& out) const
{
    byteio::putU8(out, s_.m_fifo[0]);
    byteio::putU8(out, s_.m_fifo[1]);
    byteio::putU8(out, s_.m_fifo[2]);
    byteio::putU8(out, s_.m_fifo[3]);
    byteio::putU8(out, s_.m_fifo[4]);
    byteio::putU8(out, s_.m_fifo[5]);
    byteio::putU8(out, s_.m_fifo[6]);
    byteio::putU8(out, s_.m_fifo[7]);
    byteio::putU8(out, s_.m_fifo[8]);
    byteio::putU8(out, s_.m_fifo[9]);
    byteio::putU8(out, s_.m_fifo[10]);
    byteio::putU8(out, s_.m_fifo[11]);
    byteio::putU8(out, s_.m_fifo[12]);
    byteio::putU8(out, s_.m_fifo[13]);
    byteio::putU8(out, s_.m_fifo[14]);
    byteio::putU8(out, s_.m_fifo[15]);
    byteio::putU8(out, s_.m_fifo_head);
    byteio::putU8(out, s_.m_fifo_tail);
    byteio::putU8(out, s_.m_fifo_count);
    byteio::putU8(out, s_.m_fifo_bits_taken);
    byteio::putU8(out, s_.m_previous_talk_status);
    byteio::putU8(out, s_.m_SPEN);
    byteio::putU8(out, s_.m_DDIS);
    byteio::putU8(out, s_.m_TALK);
    byteio::putU8(out, s_.m_TALKD);
    byteio::putU8(out, s_.m_buffer_low);
    byteio::putU8(out, s_.m_buffer_empty);
    byteio::putU8(out, s_.m_irq_pin);
    byteio::putU8(out, s_.m_RDB_flag);
    byteio::putU8(out, s_.m_OLDE);
    byteio::putU8(out, s_.m_OLDP);
    byteio::putU8(out, s_.m_command_register);
    byteio::putU8(out, s_.m_read_byte_register);
    byteio::putU8(out, s_.m_new_frame_energy_idx);
    byteio::putU8(out, s_.m_new_frame_pitch_idx);
    byteio::putU8(out, s_.m_new_frame_k_idx[0]);
    byteio::putU8(out, s_.m_new_frame_k_idx[1]);
    byteio::putU8(out, s_.m_new_frame_k_idx[2]);
    byteio::putU8(out, s_.m_new_frame_k_idx[3]);
    byteio::putU8(out, s_.m_new_frame_k_idx[4]);
    byteio::putU8(out, s_.m_new_frame_k_idx[5]);
    byteio::putU8(out, s_.m_new_frame_k_idx[6]);
    byteio::putU8(out, s_.m_new_frame_k_idx[7]);
    byteio::putU8(out, s_.m_new_frame_k_idx[8]);
    byteio::putU8(out, s_.m_new_frame_k_idx[9]);
    byteio::putU16(out, s_.m_current_energy);
    byteio::putU16(out, s_.m_current_pitch);
    byteio::putU16(out, s_.m_current_k[0]);
    byteio::putU16(out, s_.m_current_k[1]);
    byteio::putU16(out, s_.m_current_k[2]);
    byteio::putU16(out, s_.m_current_k[3]);
    byteio::putU16(out, s_.m_current_k[4]);
    byteio::putU16(out, s_.m_current_k[5]);
    byteio::putU16(out, s_.m_current_k[6]);
    byteio::putU16(out, s_.m_current_k[7]);
    byteio::putU16(out, s_.m_current_k[8]);
    byteio::putU16(out, s_.m_current_k[9]);
    byteio::putU16(out, s_.m_previous_energy);
    byteio::putU8(out, s_.m_subcycle);
    byteio::putU8(out, s_.m_subc_reload);
    byteio::putU8(out, s_.m_PC);
    byteio::putU8(out, s_.m_IP);
    byteio::putU8(out, s_.m_inhibit);
    byteio::putU8(out, s_.m_uv_zpar);
    byteio::putU8(out, s_.m_zpar);
    byteio::putU8(out, s_.m_pitch_zero);
    byteio::putU8(out, s_.m_c_variant_rate);
    byteio::putU16(out, s_.m_pitch_count);
    byteio::putU32(out, s_.m_u[0]);
    byteio::putU32(out, s_.m_u[1]);
    byteio::putU32(out, s_.m_u[2]);
    byteio::putU32(out, s_.m_u[3]);
    byteio::putU32(out, s_.m_u[4]);
    byteio::putU32(out, s_.m_u[5]);
    byteio::putU32(out, s_.m_u[6]);
    byteio::putU32(out, s_.m_u[7]);
    byteio::putU32(out, s_.m_u[8]);
    byteio::putU32(out, s_.m_u[9]);
    byteio::putU32(out, s_.m_u[10]);
    byteio::putU32(out, s_.m_x[0]);
    byteio::putU32(out, s_.m_x[1]);
    byteio::putU32(out, s_.m_x[2]);
    byteio::putU32(out, s_.m_x[3]);
    byteio::putU32(out, s_.m_x[4]);
    byteio::putU32(out, s_.m_x[5]);
    byteio::putU32(out, s_.m_x[6]);
    byteio::putU32(out, s_.m_x[7]);
    byteio::putU32(out, s_.m_x[8]);
    byteio::putU32(out, s_.m_x[9]);
    byteio::putU16(out, s_.m_RNG);
    byteio::putU16(out, s_.m_excitation_data);
}
bool Tms5220::loadSnapshot(const uint8_t* data, size_t size)
{
    if (!data || size != 170) return false;
    byteio::Reader r(data, size);
    State next{};
    bool valid = true;
    auto boolean = [&] { const auto value = r.u8(); if (value > 1) valid = false; return value != 0; };
    next.m_fifo[0] = uint8_t(r.u8());
    next.m_fifo[1] = uint8_t(r.u8());
    next.m_fifo[2] = uint8_t(r.u8());
    next.m_fifo[3] = uint8_t(r.u8());
    next.m_fifo[4] = uint8_t(r.u8());
    next.m_fifo[5] = uint8_t(r.u8());
    next.m_fifo[6] = uint8_t(r.u8());
    next.m_fifo[7] = uint8_t(r.u8());
    next.m_fifo[8] = uint8_t(r.u8());
    next.m_fifo[9] = uint8_t(r.u8());
    next.m_fifo[10] = uint8_t(r.u8());
    next.m_fifo[11] = uint8_t(r.u8());
    next.m_fifo[12] = uint8_t(r.u8());
    next.m_fifo[13] = uint8_t(r.u8());
    next.m_fifo[14] = uint8_t(r.u8());
    next.m_fifo[15] = uint8_t(r.u8());
    next.m_fifo_head = uint8_t(r.u8());
    next.m_fifo_tail = uint8_t(r.u8());
    next.m_fifo_count = uint8_t(r.u8());
    next.m_fifo_bits_taken = uint8_t(r.u8());
    next.m_previous_talk_status = boolean();
    next.m_SPEN = boolean();
    next.m_DDIS = boolean();
    next.m_TALK = boolean();
    next.m_TALKD = boolean();
    next.m_buffer_low = boolean();
    next.m_buffer_empty = boolean();
    next.m_irq_pin = boolean();
    next.m_RDB_flag = boolean();
    next.m_OLDE = boolean();
    next.m_OLDP = boolean();
    next.m_command_register = uint8_t(r.u8());
    next.m_read_byte_register = uint8_t(r.u8());
    next.m_new_frame_energy_idx = uint8_t(r.u8());
    next.m_new_frame_pitch_idx = uint8_t(r.u8());
    next.m_new_frame_k_idx[0] = uint8_t(r.u8());
    next.m_new_frame_k_idx[1] = uint8_t(r.u8());
    next.m_new_frame_k_idx[2] = uint8_t(r.u8());
    next.m_new_frame_k_idx[3] = uint8_t(r.u8());
    next.m_new_frame_k_idx[4] = uint8_t(r.u8());
    next.m_new_frame_k_idx[5] = uint8_t(r.u8());
    next.m_new_frame_k_idx[6] = uint8_t(r.u8());
    next.m_new_frame_k_idx[7] = uint8_t(r.u8());
    next.m_new_frame_k_idx[8] = uint8_t(r.u8());
    next.m_new_frame_k_idx[9] = uint8_t(r.u8());
    next.m_current_energy = int16_t(r.u16());
    next.m_current_pitch = int16_t(r.u16());
    next.m_current_k[0] = int16_t(r.u16());
    next.m_current_k[1] = int16_t(r.u16());
    next.m_current_k[2] = int16_t(r.u16());
    next.m_current_k[3] = int16_t(r.u16());
    next.m_current_k[4] = int16_t(r.u16());
    next.m_current_k[5] = int16_t(r.u16());
    next.m_current_k[6] = int16_t(r.u16());
    next.m_current_k[7] = int16_t(r.u16());
    next.m_current_k[8] = int16_t(r.u16());
    next.m_current_k[9] = int16_t(r.u16());
    next.m_previous_energy = uint16_t(r.u16());
    next.m_subcycle = uint8_t(r.u8());
    next.m_subc_reload = uint8_t(r.u8());
    next.m_PC = uint8_t(r.u8());
    next.m_IP = uint8_t(r.u8());
    next.m_inhibit = boolean();
    next.m_uv_zpar = boolean();
    next.m_zpar = boolean();
    next.m_pitch_zero = boolean();
    next.m_c_variant_rate = uint8_t(r.u8());
    next.m_pitch_count = uint16_t(r.u16());
    next.m_u[0] = int32_t(r.u32());
    next.m_u[1] = int32_t(r.u32());
    next.m_u[2] = int32_t(r.u32());
    next.m_u[3] = int32_t(r.u32());
    next.m_u[4] = int32_t(r.u32());
    next.m_u[5] = int32_t(r.u32());
    next.m_u[6] = int32_t(r.u32());
    next.m_u[7] = int32_t(r.u32());
    next.m_u[8] = int32_t(r.u32());
    next.m_u[9] = int32_t(r.u32());
    next.m_u[10] = int32_t(r.u32());
    next.m_x[0] = int32_t(r.u32());
    next.m_x[1] = int32_t(r.u32());
    next.m_x[2] = int32_t(r.u32());
    next.m_x[3] = int32_t(r.u32());
    next.m_x[4] = int32_t(r.u32());
    next.m_x[5] = int32_t(r.u32());
    next.m_x[6] = int32_t(r.u32());
    next.m_x[7] = int32_t(r.u32());
    next.m_x[8] = int32_t(r.u32());
    next.m_x[9] = int32_t(r.u32());
    next.m_RNG = uint16_t(r.u16());
    next.m_excitation_data = int16_t(r.u16());
    if (!valid || next.m_fifo_head >= 16 || next.m_fifo_tail >= 16 || next.m_fifo_count > 16 ||
        next.m_fifo_bits_taken >= 8 || next.m_IP > 7 || next.m_PC > 12 ||
        next.m_subcycle > 2 || next.m_subc_reload != 1 || next.m_new_frame_energy_idx > 15 ||
        next.m_new_frame_pitch_idx > 63 || next.m_c_variant_rate > 15 || next.m_pitch_count > 511 ||
        next.m_current_pitch < 0 || next.m_current_pitch > 511 || next.m_current_energy < 0 ||
        next.m_current_energy > 1023 || next.m_previous_energy > 1023) return false;
    for (int i = 0; i < 10; ++i)
        if (next.m_new_frame_k_idx[i] >= (1 << m_coeff->kbits[i]) ||
            next.m_current_k[i] < -512 || next.m_current_k[i] > 511) return false;
    for (auto value : next.m_x) if (value < -1048576 || value > 1048576) return false;
    for (auto value : next.m_u) if (value < -1048576 || value > 1048576) return false;
    s_ = next;
    return true;
}

}
#undef LOGMASKED
#undef INTERP_SHIFT
#undef TMS5220_HAS_RATE_CONTROL
