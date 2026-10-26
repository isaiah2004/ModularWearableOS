"""Source regressions for the power gap fixes that cannot run on the host (G11/G13/G14/G99, wakers)."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def src(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")


class GapFixPowerSource(unittest.TestCase):
    def test_battery_percent_unknown_until_estimated(self):
        hal = src("firmware/main/hal_esp32.c")
        self.assertIn("s_power={.battery_percent=-1", hal)  # G13: no invented 50 %

    def test_off_screen_wakers_ride_the_grid(self):
        hal = src("firmware/main/hal_esp32.c")
        self.assertIn("BLE_ADV_SLOW_MIN 800", hal)
        self.assertIn("atomic_load(&s_air_reserved)?100u:sleep_grid_delay_ms(", hal)
        self.assertIn("sleep_grid_notify_wait(1000)", src("firmware/main/imu_hal.c"))
        self.assertIn("sleep_grid_notify_wait(1000)", src("nos/sports.c"))
        self.assertIn("xQueueReceive(s_flush_q,&j,portMAX_DELAY)", src("firmware/main/nos_main.c"))

    def test_raw_journal_is_gated_and_pruned(self):
        sp = src("nos/sports.c")
        self.assertIn("raw_type(r->type) && !raw_open()", sp)  # G11 gate in the producer path
        self.assertIn("sports_prune_raw(", sp)
        self.assertIn("IMU_JOURNAL_MS", sp)
        self.assertIn("sports_raw_set(SPORTS_RAW_WORKOUT", src("nos/svc.c"))

    def test_main_loop_has_critical_battery_and_keep_awake_cap(self):
        m = src("firmware/main/nos_main.c")
        self.assertIn("critical_battery_tick(&policy_power,nowms)", m)  # G14
        self.assertIn("power_keep_expired(idle,power_wrist_down(", m)  # G99
        self.assertIn("pending!=s_keep_released", m)
        self.assertIn("power_torch_state(", src("nos/apps/con_torch.c"))  # G29
        self.assertIn("SCR_SELFCAP", src("nos/apps/con_torch.c"))

    def test_usb_power_is_not_labelled_charging(self):
        self.assertNotIn('"CHARGING"', src("nos/apps/sys_power.c"))  # G12


if __name__ == "__main__":
    unittest.main()
