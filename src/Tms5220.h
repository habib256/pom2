// POM2 — adapted from MAME, BSD-3-Clause.
// Copyright Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu, Raphael Nabet,
// Couriersud, Michael Zapf. See THIRD-PARTY.md and Tms5220Tables.h.
// MAME src/devices/sound/tms5220.cpp:960-1370,1561-1662 (2026-10-18).
// TMS5220C: native clock/80 synthesis, patent interpolation/lattice/DAC.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace pom2 {
class Tms5220 {
public:
    Tms5220() { reset(); }
    void reset();
    void write(uint8_t value);
    uint8_t status();
    void process(int16_t* samples, unsigned count);
    bool ready() const { return !s_.m_DDIS || s_.m_fifo_count < 16; }
    void appendSnapshot(std::vector<uint8_t>& out) const;
    bool loadSnapshot(const uint8_t* data, size_t size);
private:
    struct State {
uint8_t m_fifo[16];
uint8_t m_fifo_head, m_fifo_tail, m_fifo_count, m_fifo_bits_taken;
bool m_previous_talk_status, m_SPEN, m_DDIS, m_TALK, m_TALKD, m_buffer_low, m_buffer_empty;
bool m_irq_pin, m_RDB_flag, m_OLDE, m_OLDP;
uint8_t m_command_register, m_read_byte_register;
uint8_t m_new_frame_energy_idx, m_new_frame_pitch_idx, m_new_frame_k_idx[10];
int16_t m_current_energy, m_current_pitch, m_current_k[10];
uint16_t m_previous_energy;
uint8_t m_subcycle, m_subc_reload, m_PC, m_IP;
bool m_inhibit, m_uv_zpar, m_zpar, m_pitch_zero;
uint8_t m_c_variant_rate;
uint16_t m_pitch_count;
int32_t m_u[11], m_x[10];
uint16_t m_RNG;
int16_t m_excitation_data;
    } s_{};
    void update_fifo_status_and_ints();
    int read_bits(int count);
    void parse_frame();
    int16_t clip_analog(int16_t value) const;
    int32_t matrix_multiply(int32_t a, int32_t b) const;
    int32_t lattice_filter();
    static void printbits(long, int) {}
    bool talk_status() const { return s_.m_SPEN || s_.m_TALKD; }
    bool old_frame_silence_flag() const { return s_.m_OLDE; }
    bool old_frame_unvoiced_flag() const { return s_.m_OLDP; }
    bool new_frame_stop_flag() const { return s_.m_new_frame_energy_idx == 15; }
    bool new_frame_silence_flag() const { return s_.m_new_frame_energy_idx == 0; }
    bool new_frame_unvoiced_flag() const { return s_.m_new_frame_pitch_idx == 0; }
};
}
