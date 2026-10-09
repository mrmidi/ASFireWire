"""Deterministic behavioral checks for the hardware-free SPH simulator."""
import unittest
from types import SimpleNamespace
from compare_sph import (Packet, Replay, TICKS_PER_SECOND, decode_sph, encode_sph,
                         simulate, SphServo, Q32, trunc_div, absolute_phase_seed)


class TimingTests(unittest.TestCase):
    def test_replay_translates_offsets_across_second_wrap(self):
        incoming = Packet(7999, 0, [encode_sph(TICKS_PER_SECOND + 700)])
        outgoing = Replay().emit(incoming, 8031)
        self.assertEqual(decode_sph(outgoing[0]), (8032 * 3072 + 700) % TICKS_PER_SECOND)

    def test_all_rates_track_clean_clock_and_loss_stops(self):
        for rate in (44100,48000,88200,96000,176400,192000):
            with self.subTest(rate=rate):
                args = SimpleNamespace(seed=1, seconds=.1, rate=rate, start_cycle=7990,
                    lead_cycles=2, jitter_ticks=8, drift_ppm=100, delay_cycles=32,
                    window_frames=512, clamp_ppm=2000, locked_shift=2, acquisition_shift=0)
                _, stats = simulate(args, 'steady')
                self.assertGreater(stats['replay']['frames'], 0)
                self.assertLess(stats['replay']['peak_error_us'], .021)
                self.assertLess(stats['synthesis']['peak_error_us'], 3)
                self.assertLessEqual(abs(stats['synthesis']['frames']-stats['replay']['frames']), 32)
                self.assertEqual(stats['synthesis_resyncs'], 0)
                self.assertIsNotNone(stats['synthesis_lock_frame'])
                _, dropped = simulate(args, 'drop')
                self.assertTrue(dropped['stopped_on_discontinuity'])
                self.assertLess(dropped['replay']['frames'], stats['replay']['frames'])


class Pr172RegressionTests(unittest.TestCase):
    # Observed data, attributed to PR #172 MotuV3SphWireParityTests.cpp.
    CAPTURED = (
        0x01072000, 0x011ddbff, 0x01349bff, 0x014b5bff,
        0x0161e125, 0x0177a50d, 0x018e2235, 0x01a5d177,
        0x01bd0775, 0x01d3c785, 0x01ea8795, 0x01f3f39c,
        0x0104a4de, 0x011be4ef, 0x013324ff, 0x014a6510,
        0x0161a520, 0x0178e530, 0x01902541, 0x01a76551,
        0x01bea561, 0x01d5e572, 0x01ed2582, 0x01f3f987)

    def test_official_driver_captured_quadlets_round_trip(self):
        for stamp in self.CAPTURED:
            self.assertEqual(encode_sph(decode_sph(stamp)), stamp)
            self.assertEqual(stamp >> 25, 0)

    def test_host_seconds_never_reach_wire(self):
        for seconds in range(130):
            self.assertEqual(encode_sph(seconds*TICKS_PER_SECOND+12345), encode_sph(12345))
        self.assertEqual(decode_sph(encode_sph(-1)), TICKS_PER_SECOND-1)

    def test_elapsed_interval_normalizes_locked_gain(self):
        for frames in (512,1024,2048):
            servo = SphServo(48000, clamp_ppm=100000)
            servo.update(1,0,0,0,seed_ticks=100)
            servo.locked = True
            servo.update(1,frames,frames*512,0)
            applied = (servo.step-servo.nominal)*frames
            self.assertLessEqual(abs(applied-25*Q32), frames)

    def test_acquisition_corrects_whole_seed(self):
        servo = SphServo(48000, clamp_ppm=100000)
        servo.update(1,0,0,0,seed_ticks=3072)
        servo.update(1,512,512*512,0)
        self.assertEqual((servo.step-servo.nominal)*512, 3072*Q32)

    def test_strict_hard_resync_boundary(self):
        for seed, expected in ((12288,False),(12289,True),(-12289,True)):
            servo = SphServo(48000, clamp_ppm=100000)
            servo.update(1,0,0,0,seed_ticks=seed)
            servo.update(1,512,512*512,0)
            self.assertEqual(servo.hard_resync, expected)
            if expected:
                self.assertEqual(servo.step,servo.nominal)
                self.assertFalse(servo.locked)

    def test_lock_and_generation_regression_stall(self):
        servo = SphServo(48000)
        servo.update(1,0,0,0)
        servo.update(1,512,512*512,0)
        self.assertTrue(servo.locked)
        self.assertFalse(servo.update(1,512,512*512,0))
        self.assertEqual(servo.cause,'bridge-stall')
        servo.update(2,0,0,0)
        self.assertEqual(servo.cause,'generation-change')
        self.assertEqual(servo.step,servo.nominal)
        self.assertEqual(servo.error,0)

    def test_actual_tx_actuation_cancels_rx_drift(self):
        servo = SphServo(48000)
        servo.update(1,0,0,0)
        servo.update(1,512,512*512+20,20*Q32)
        self.assertEqual(servo.error,0)
        self.assertTrue(servo.locked)
        self.assertEqual(servo.step,((512*512+20)*Q32)//512)

    def test_absolute_seed_sign_setpoint_and_branch_cut(self):
        self.assertEqual(absolute_phase_seed(510,510),0)
        self.assertEqual(absolute_phase_seed(-887,-887),1397)
        center = 510+1536
        self.assertEqual(abs(absolute_phase_seed(center+1)-absolute_phase_seed(center-1)),3070)
        self.assertEqual(abs(absolute_phase_seed(center+1,center)-absolute_phase_seed(center-1,center)),2)
        for center in range(-6144,6145,97):
            for offset in range(-1600,1601,53):
                self.assertLessEqual(abs(absolute_phase_seed(center+offset,center)),3072)

    def test_lock_boundary_and_late_absolute_seed_preserves_lock(self):
        for seed in (61,-61,62,-62):
            servo = SphServo(48000)
            servo.update(1,0,0,0,seed_ticks=seed)
            servo.update(1,512,512*512,0)
            self.assertEqual(servo.locked,abs(seed)==61)
        servo = SphServo(48000)
        servo.update(1,0,0,0)
        servo.update(1,512,512*512,0)
        servo.update(1,1024,1024*512,0,absolute_phase_ticks=900,phase_center_ticks=900)
        self.assertTrue(servo.locked)
        self.assertEqual(servo.error,-390*Q32)
        servo.update(1,1536,1536*512,0)
        self.assertEqual(servo.step-servo.nominal,trunc_div(-390*Q32,512*4))

    def test_reference_regressions_and_per_observation_flags(self):
        for frames,ticks,cause in ((511,512*512,'frame-regression'),
                                  (513,512*512-1,'tick-regression'),
                                  (513,512*512,'non-advancing-ticks')):
            servo = SphServo(48000)
            servo.update(1,0,0,0)
            servo.update(1,512,512*512,0)
            self.assertFalse(servo.update(1,frames,ticks,0))
            self.assertEqual(servo.cause,cause)
            self.assertEqual(servo.step,servo.nominal)
        servo = SphServo(48000)
        servo.update(1,0,0,0,seed_ticks=12289)
        servo.update(1,512,512*512,0)
        self.assertTrue(servo.hard_resync)
        servo.update(1,512,512*512,0)
        self.assertFalse(servo.hard_resync)
        self.assertFalse(servo.clamped)

    def test_clamp_bounds_acquisition(self):
        servo = SphServo(48000,clamp_ppm=100)
        servo.update(1,0,0,0,seed_ticks=3000)
        servo.update(1,512,512*512,0)
        self.assertTrue(servo.clamped)
        self.assertEqual(servo.step,servo.upper)

    def test_fractional_period_does_not_truncate_to_integer_ticks(self):
        step = SphServo(44100).nominal
        ticks = (step*44100)//Q32
        self.assertLessEqual(abs(ticks-TICKS_PER_SECOND),1)
        self.assertNotEqual((step//Q32)*44100,TICKS_PER_SECOND)
        self.assertEqual(trunc_div(-7,4),-1)

    def test_drift_converges_even_with_late_updates(self):
        for rate in (44100,48000,88200,96000,176400,192000):
            for frames in (512,2048):
                servo = SphServo(rate)
                servo.update(1,0,0,0)
                correction = 0
                for update in range(1,81):
                    correction += (servo.step-servo.nominal)*frames
                    ticks = round(update*frames*TICKS_PER_SECOND/(rate*(1+14e-6)))
                    servo.update(1,update*frames,ticks,correction)
                self.assertLess(abs(servo.error/Q32),3)
                self.assertTrue(servo.locked)


if __name__ == '__main__':
    unittest.main()
