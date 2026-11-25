"""
Power consumption estimate for the Smiley feedback panel.

Current: ESP32 Series Datasheet v5.3 (Espressif, 2026-07)
         - Table 4-2 "Power Consumption by Power Modes"
         - Table 5-4 "Current Consumption Depending on RF Modes" (3.3 V, 25 °C, TX 50 % duty cycle)
Times:   our own measurements from the serial log, 24-25 Nov 2026 (see docs/portfolio.md, section 8)

NOTE: The datasheet values cover ONLY the ESP32 chip. The DevKit board's USB chip,
voltage regulator and power LED are not included. Replace the values with your own
measurements when available and run the script again:   python analyse/power_estimate.py
"""
import random

# ---------------------------------------------------------------- current (mA)
I_RX           = 97.5    # Table 5-4: Receive 802.11b/g/n, 95 ~ 100 mA
I_TX_MAX       = 240     # Table 5-4: Transmit 802.11b 1 Mbps +19.5 dBm (short peaks only)
I_MODEM_LOW    = 30      # Table 4-2: Modem-sleep, 240 MHz, dual-core: 30 mA ~ 68 mA
I_MODEM_HIGH   = 68
I_DEEP         = 0.010   # Table 4-2: Deep-sleep, RTC timer + RTC memory: 10 µA

# Assumptions (not from the datasheet):
I_CONNECT_LOW, I_CONNECT_HIGH = 100, 130   # WiFi connection + TLS: mostly RX, short TX peaks
I_LED = 3.5            # LED via 220 Ω from 3.3 V: approx. 1.4 mA (Vf 3.0 V) .. 5.9 mA (Vf 2.0 V)

# Mid estimate used in the daily simulation
I_CONNECT = (I_CONNECT_LOW + I_CONNECT_HIGH) / 2      # 115 mA
I_IDLE    = (I_MODEM_LOW + I_MODEM_HIGH) / 2          # 49 mA

# ---------------------------------------------------------------- times (s), measured
T_WIFI    = 1.16       # wake-up -> WiFi connected (avg. with TLS, 25/11)
T_TLS     = 0.63       # TLS handshake + MQTT login (new firmware, 25/11)
T_CONNECT = T_WIFI + T_TLS
T_NTP     = 2.0        # avg. wait for NTP after MQTT is connected
T_LOCK    = 7.0        # required

ROUTINES_PER_DAY = 4   # ROUTINE_SLEEP_S = 6 h
BATTERY_MAH = 2000


def mAh(mAs):
    return mAs / 3600


def break_even(i_connect, i_idle):
    """Seconds after the lock where staying awake costs the same as sleeping and waking up again.
    Extra energy for waking up = (I_connect - I_idle) * T_connect (the rest of the lock is the same)."""
    e_extra = (i_connect - i_idle) * T_CONNECT
    return e_extra / (i_idle - I_DEEP), e_extra


def simulate_day(press_times, idle_s):
    """Returns (wake-ups, awake time s, accepted presses, ignored presses, charge mAs)."""
    wakeups = accepted = ignored = 0
    awake = 0.0
    lock_end = -1e9
    awake_until = -1e9
    start = None
    for t in sorted(press_times):
        if t < lock_end:
            ignored += 1
            continue
        accepted += 1
        if t > awake_until:                    # asleep -> new wake-up
            if start is not None:
                awake += awake_until - start
            wakeups += 1
            start = t
        lock_end = t + T_LOCK
        awake_until = lock_end + idle_s
    if start is not None:
        awake += awake_until - start
    routine_mas = ROUTINES_PER_DAY * (T_CONNECT * I_CONNECT + T_NTP * I_IDLE)
    asleep = 24 * 3600 - awake - ROUTINES_PER_DAY * (T_CONNECT + T_NTP)
    charge = (wakeups * (I_CONNECT - I_IDLE) * T_CONNECT   # extra for connecting
              + awake * I_IDLE                             # awake (incl. lock)
              + accepted * T_LOCK * I_LED                  # LED during lock
              + routine_mas
              + asleep * I_DEEP)
    return wakeups, awake, accepted, ignored, charge


def scenario(name, rnd):
    t = []
    if name == "A":            # 20 presses spread over 08-16
        t = [rnd.uniform(8, 16) * 3600 for _ in range(20)]
    elif name == "B":          # 300 presses: 100 spread over 08-16 + 200 at lunch 11:30-12:30
        t = [rnd.uniform(8, 16) * 3600 for _ in range(100)]
        t += [rnd.uniform(11.5, 12.5) * 3600 for _ in range(200)]
    elif name == "C":          # 1000 presses spread over 08-18 (approx. 1 per 36 s)
        t = [rnd.uniform(8, 18) * 3600 for _ in range(1000)]
    return t


if __name__ == "__main__":
    print("Break-even (s after the lock):")
    for i_c in (I_CONNECT_LOW, I_CONNECT_HIGH):
        for i_i in (I_MODEM_LOW, I_MODEM_HIGH):
            be, e = break_even(i_c, i_i)
            print(f"  I_connect {i_c:>3} mA, I_idle {i_i:>2} mA: E_extra {e:6.1f} mAs -> {be:4.1f} s")

    print("\nOne isolated wake-up (press -> asleep again):")
    for idle in (0, 5, 10, 30, 60):
        awake = T_LOCK + idle
        q = T_CONNECT * I_CONNECT + (awake - T_CONNECT) * I_IDLE + T_LOCK * I_LED
        print(f"  IDLE {idle:>2} s: awake {awake:4.0f} s, {q:6.0f} mAs = {mAh(q):.3f} mAh")

    print(f"\nPer day (avg. of 200 simulated days), ESP32 chip only, battery {BATTERY_MAH} mAh:")
    print("| Scenario | IDLE | Wake-ups | Awake per day | Ignored presses | mAh per day | Battery (days) |")
    print("|---|---|---|---|---|---|---|")
    for name in ("A", "B", "C"):
        for idle in (0, 5, 10, 30, 60):
            rnd = random.Random(42)
            res = [simulate_day(scenario(name, rnd), idle) for _ in range(200)]
            w, a, acc, ig, q = (sum(x[i] for x in res) / len(res) for i in range(5))
            print(f"| {name} | {idle} s | {w:.0f} | {a/60:.0f} min | {ig:.0f} | {mAh(q):.1f} | {BATTERY_MAH/mAh(q):.0f} |")
