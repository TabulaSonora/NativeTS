#include "tabulasonora/part.hpp"

namespace ts {

void Part::reset()
{
    program = 0;
    bank = 0;
    bank_lsb = 0;
    rx = RxSwitches{};
    pan = sequence_builder::default_pan;
    modulation = 0;
    damper = 0;
    soft = false;
    channel_pressure = 0;
    poly_pressure.fill(0);
    cc1_number = 16;
    cc2_number = 17;
    cc1 = 0;
    cc2 = 0;
    xg_bank_msb = -1;
    reverb_send = sequence_builder::default_reverb_send;
    chorus_send = sequence_builder::default_chorus_send;
    delay_send = 0;
    bend = 8192;
    bend_range = 2;
    control.reset();
    fine_tune = 0x2000;
    coarse_tune = 0x40;
    key_shift = 0x40;
    vibrato_rate = 0x40;
    vibrato_depth = 0x40;
    vibrato_delay = 0x40;
    tvf_cutoff = 0x40;
    tvf_resonance = 0x40;
    env_attack = 0x40;
    env_decay = 0x40;
    env_release = 0x40;
    scale_tuning.fill(0x40);
    rhythm = -1;
    key_low = 0;
    key_high = 0x7F;
    eq_enabled = false;
    efx_enabled = false;
    envelope_delay = 0x40;
    envelope_delay_tone = 0x40;
    velocity_depth = 0x40;
    velocity_offset = 0x40;
    pitch_offset_fine = 0x08;
    rpn_msb = 0x7F;
    rpn_lsb = 0x7F;
    nrpn_msb = 0x7F;
    nrpn_lsb = 0x7F;
    data_entry_is_nrpn = false;
    drum_keys.reset();
    sustained.clear();
    portamento_time = 0;
    portamento_on = false;
    mono = false;
    portamento_control_key = -1;
    last_key = -1;
    sostenuto_down = false;
    sostenuto_captured.clear();
    sostenuto_released.clear();

    volume_ = sequence_builder::default_volume;
    expression_ = sequence_builder::default_expression;
    master_ = sequence_builder::default_master;
    recompute();
}

} // namespace ts
