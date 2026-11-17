# In-circuit capacitor checker - 2-WIRE variant (ESP32-C3 0.42" OLED, no op-amp)

## Wiring - schematic/two_wire_front_end.png (source: schematic/draw_schematic.py)
The drawing script reads the connections back out of the drawn geometry and fails if they differ from `schematic/netlist.py`;
the simulation (`sim/chain_ac.py`) is built from the same netlist. Drawing = netlist = simulation.

| Part | Value | Between |
|---|---|---|
| R1 | 330 ohm | GPIO3 and node A |
| R2 | 10 ohm 1 % | node A and GND |
| clip + / clip - | | node A / GND (capacitor under test goes here) |
| C1 | 10 uF electrolytic, 16 V or more, **+ toward R5** (4.7 to 47 uF all work) | R5 (+) and node A (-) |
| R5 | 2.2 k | C1 and GPIO0 (ADC in) |
| R6 | 10 k | GPIO0 and VB |
| R7 | 47 k | 3V3 and VB |
| R8 | 10 k | VB and GND (VB = 0.579 V; 5.6 k also works, VB = 0.351 V) |
| C2 | 10 uF electrolytic or ceramic, + on VB | VB and GND |
| R3 | 5.6 k | GPIO3 and REF (GPIO4) |
| R4 | 1 k | REF (GPIO4) and GND |

No diodes. Parts: 330, 10 (1 %), 5.6k, 1k, 2.2k, 10k (x2: R6, R8), 47k resistors, 2x 10 uF, 2 leads/clips.
Board pins used: GPIO0 (ADC1_CH0), GPIO3 (LEDC drive), GPIO4 (ADC1_CH4), 3V3, GND, plus the on-board OLED and BOOT button (GPIO9).
Check your board variant: firmware assumes OLED SDA=GPIO5, SCL=GPIO6 (01Space layout). Some 0.42" boards use SDA=GPIO8, SCL=GPIO9:
add `-DOLED_SDA=8 -DOLED_SCL=9` to build_flags in platformio.ini. GPIO0, 3 and 4 must be broken out on your board.

### Perfboard build (individual pads, 6 columns x 8 rows) - `perfboard/perfboard.png`, `perfboard/BUILD.txt`
Resistors lie flat in a stack; 9 solder bridges between adjacent pads and 3 insulated back-side wires join the nets (checker: `perfboard/perf.py`).

### Stripboard build (only if you have a board with copper strips: 4 strips x 12 holes, 30 x 10 mm)
`stripboard/stripboard_4x12.png` (component side + copper side), step list in `stripboard/BUILD.txt`, checker `stripboard/layout.py`
(reads the nets back from strips, cuts and jumper and compares with netlist.py), search program `stripboard/solve.py`.
Alternative with all MCU wires on the top strip and probe pads at the right end: `stripboard/stripboard_5x14_header.png` (5 x 14, 8 jumpers,
`BUILD_header.txt`). Electrically verified, but the long jumpers must be insulated wire on the copper side.
Parts stand upright. Use electrolytic cans of 5 mm diameter or less, or lay them down if larger.

### Test probes (3D-printed parts) - `cad/`
Probe part for the two test leads, designed in SolidWorks (about 10 x 52.5 x 10 mm).
* `cad/Multimeter_probes.SLDPRT` - SolidWorks source (edit this one)
* `cad/Multimeter_probes.STL` - mesh for any slicer
* `cad/Multimeter_probes.3mf` - Bambu Studio / OrcaSlicer project with the print settings

Keep the leads short and thick (20 AWG, under 15 cm) and the contact firm: lead and contact resistance adds to ESR, and the Calibrate tab (step 2, clips shorted) only removes what is constant between calibration and use.

### Why there are no diodes (changed from the first version)
The earlier drawing had a 5.1 V zener across the clips and two 1N5819 clamps on the ADC pin. Dropped:
* A 1N5819 leaks on the order of 10 uA at 3 V (datasheet max is 1 mA at 40 V and 10 mA at 100 C, doubling every ~10 C). The ADC node sits behind
  ~17 kohm of bias resistance, so that leakage moves the bias point by 100 mV or more and can push the ADC out of range when warm.
* Anything with leakage or a V-dependent conductance at node A would load the 10 ohm shunt differently when the clips are open (calibration) and
  when a cap is attached, which corrupts small-capacitor readings.
* The ESP32's own pad diodes plus R5 (2.2 k) already limit current into GPIO0, and R1 (330 ohm) does the same for GPIO3.
* C1 is an electrolytic with + on the ADC side (the ADC node sits at 0.58 V, node A near 0 V, so the polarity is right in normal use).
  If a charged cap is ever clipped on, C1 can be reverse-biased for a moment: cheap to replace. For more robustness use two electrolytics
  back-to-back (both + together or both - together), e.g. 2 x 22 uF = 11 uF. A leaky old electrolytic shifts the bias slightly: use a fresh one.
Optional later: a 5.1 V zener (1N4733A, DO-41) across the clips is harmless to accuracy and gives extra protection.

### Safety
Always power off, unplug and discharge the board and the cap before clipping on (check with a multimeter). Up to ~12 V of residual charge on a
normal cap is survivable by design (R2, R1, R5 limit the current); 25 V and up on a big cap can damage R2 or the pins; mains-side bulk caps can
hold lethal voltage and must never be connected.

### Verification done (this version)
* Drawing read back from geometry == netlist (no unmarked junctions, no crossings).
* Exact AC model of this netlist: VB = 0.579 V; pad/ref gain 0.157 (10 Hz) to 0.159 (1-8 kHz) with clips open; node A swing 97 mV p-p open.
  ADC signal levels sit mid-range at 2.5 dB attenuation (pad 0.53-0.63 V, REF 0-0.5 V).
* The firmware DSP (`capdsp.h`) run on captures simulated from this exact netlist: 12 scenarios give the expected verdicts.
* C1 sweep 2.2 to 100 uF in the simulation: verdicts and C/ESR unchanged (C1 cancels in the open-clip calibration). Start-up DC-step
  settling (tau = C1 x ~19 kohm, 192 ms at 10 uF) adds <0.1 % error up to 47 uF; stay between 4.7 and 47 uF.
* NOT done: building/running on real hardware (see Status).

## Build and flash (PlatformIO)
You need a computer with internet access (the first build downloads ~1 GB of toolchain), a USB-C **data** cable, and the finished circuit (flashing works
without it; the OLED will just say NO SIGNAL when you test).
1. **Install PlatformIO:** either VS Code + the "PlatformIO IDE" extension, or the command line: `pip install platformio`.
2. **Put the project in a folder whose full path has NO spaces** (e.g. `C:\dev\capchecker`, not `C:\Users\me\Coding Projects\...`): ESP-IDF refuses to build otherwise ("Detected a whitespace character in project paths").
   **Open that folder** (the one containing `platformio.ini`; this README is in it). In VS Code: File > Open Folder, then wait for PlatformIO to finish its setup.
   `platformio.ini` pins `espressif32@^6.9.0` (ESP-IDF 5.3 or newer): do not downgrade, older versions lack `driver/i2c_master.h`.
3. **Connect the board** with USB-C. It shows up as a USB serial/JTAG device (no driver needed on Windows 10+, macOS or Linux; on Linux you may need
   `sudo usermod -aG dialout $USER` once, then log out and in).
4. **Build and upload:** PlatformIO toolbar > Upload (arrow icon), or in a terminal in the project folder: `pio run -t upload`.
   If it says "failed to connect" or cannot find the port, put the chip into download mode: **hold BOOT, tap RESET (or plug in the USB cable while holding BOOT),
   release BOOT**, then upload again. After a successful upload press RESET once. GPIO9 (BOOT) is also the test button: do not hold it while the board powers up or resets.
5. **Watch the log** (optional): `pio device monitor` (115200 baud). At start-up it prints the Wi-Fi name, password and address; Ctrl+C to leave.
6. **First start:** the OLED shows the Wi-Fi name, password and address, then NEED CAL. Tap the button for the guided calibration (clips apart, then clips together), or join Wi-Fi `CapChecker` (password `capcheck1`) with your phone, open `http://192.168.4.1` (turn mobile data off if the
   page will not load) and follow the Calibrate tab.

Build options go in `build_flags` in `platformio.ini`: `-DOLED_SDA=8 -DOLED_SCL=9` (other board variant), `-DOLED_XOFF=.. -DOLED_YOFF=..` (starting position of the picture; normally use SCREEN FIT on the device instead),
`-DAP_SSID=\"MyName\" -DAP_PASS=\"mypassword1\"` (Wi-Fi name/password, at least 8 characters), `-DWIFI_ENABLED=0` (no Wi-Fi and no web page).
To wipe the stored calibration and learned parts: `pio run -t erase`, then upload again (or "Erase calibration" on the web page).

The firmware has been built with PlatformIO (espressif32 6.9.0, ESP-IDF 5.3.1) on Windows. If a build fails on another IDF version, a compiler error mentioning `adc_continuous`, `i2c_master`, `esp_http_server` or `esp_wifi` usually means a renamed field: send me the first few lines of the error.
Blank OLED: wrong SDA/SCL (see above). Nothing happens on a test: check the log for "OLED init failed", and that GPIO0/3/4 match your wiring.

## Use
**Wi-Fi:** the device shows how to connect on its screen at power-up (and any time: hold the button, WIFI INFO). Join Wi-Fi `CapChecker`,
password `capcheck1` (change with `-DAP_SSID/-DAP_PASS`), open `http://192.168.4.1` (no internet on that network: if the page does not load, turn mobile data off).
Everything can be done from the web page; results also appear on the OLED. Power off and fully discharge the board and the cap before clipping on.

**Starting a test (button or web): you no longer have to be fast.** Three start modes (menu GO, or the selector on the web page):
- *When the probes are on the part* (default): press start, then clip on. The checker probes the clips about 5 times a second and starts only after
  3 consistent probes spanning at least 1 s show a part connected (|Z| at 1 kHz below 1 k ohm) - a clip that is only touching or wiggling keeps resetting the count. Gives up after 60 s.
  A capacitor below about 0.16 uF reads above 1 k ohm and is not detected: use the countdown mode for those.
- *After a 5 s countdown*: press start, get your hands on the probes.
- *Immediately*.
Any button press (or Cancel on the web page) aborts the wait or the measurement.

**OLED and the single button.** TAP = main action, HOLD (0.7 s) = menu / accept / back, HOLD 10 s = SCREEN FIT.
- Home: big nominal value. TAP = test, HOLD = menu. The bottom line alternates with the web address. If not calibrated it says NEED CAL: TAP starts the guided calibration.
- Menu (TAP = next, HOLD = choose): NOM (pick the printed value), ACC (Quick/Normal/Precise), GO (ON CAP / IN 5S / NOW), WIFI INFO (3 pages: network and password, address, mobile-data tip), CALIBRATE (guided: clips apart with a 5 s countdown, then clips together - starts when it feels the contact), SCREEN FIT, BACK.
- Measuring: progress bar; "CLIP PROBES ON THE CAP" with a stability bar while waiting.
- Result: verdict bar, big capacitance, ESR and error bars; a second page (reference, nominal, ESR x C, reads) alternates every 3 s. TAP = test again, HOLD = menu.
- **SCREEN FIT** (menu, or HOLD 10 s, or the Screen position card on the web page): a frame is drawn around the window; adjust Y offset, X offset and flip
  until the whole frame is visible. Saved in flash. If the top is still cut off, raise Y (range 0-24).

### Calibration (Calibrate tab; each step stores its result in flash)
1. **Clips open** (about a minute): reference with nothing connected, plus the noise floor of every frequency. Redoing it clears steps 2-4.
2. **Clips shorted**: removes the lead and clip-contact resistance from every reading. Redo when you change leads.
3. **Known resistor** (optional, recommended): 1 % resistor of 2.2-10 ohm (4.7 is good) across the clips; sets the gain (ESR accuracy). Rejected if the reading is >25 % from the value you entered.
4. **Known capacitor** (optional): a part whose value you trust (film/C0G). One of at most 3 uF and one above; between them the correction is interpolated. Rejected if >30 % off.
Without steps 3/4 the readings still work; the error bars are simply wider (3 % gain and 3 % C allowance instead of 1 % and 1.5 %).

### Error bars and verdict confidence
Every result shows C and ESR with a 1-sigma uncertainty: statistical (repeat captures and the calibrated noise floor, propagated through the
fit, scaled up if the data do not follow ESR + 1/(jwC)), the noise of the stored open calibration, a 30 mohm clip-contact allowance on ESR, and
the gain/C-scale allowance above. The checker repeats captures at the most informative frequency until C is within 2 % and ESR within 0.03 ohm
statistically (or the budget is spent / the error stops shrinking). **Accuracy** setting: Quick = 1 capture per frequency, Normal = 2+ with up to 28
extra, Precise = 3+ with up to 60 extra. A verdict ending in **?** (BORDERLINE on the OLED) means the error bars reach into a neighbouring verdict.

### Learned good parts (Good parts tab)
Measure parts you know are good: pick the nominal value, press *Measure and add*. Later tests with the same nominal value (or AUTO, if C is
within 30 %) are judged against that reference: ESR above 1.5x = WEAK, 2.5x = BAD; C below 90 % / 80 % of the good part = WEAK / BAD.
Several good parts of one value are averaged. Without a reference the older rules apply: C vs nominal (80 % / 135 %) and ESR x C (250 / 600 ohm.uF).

## Test frequencies
10, 30, 100, 300 Hz, 1, 3, 8, **12, 15 kHz**. The two new high frequencies make small capacitors measurable (0.47 uF has 23 ohm reactance at 15 kHz).
15 kHz is the practical ceiling: the ESP32-C3 ADC delivers at most ~83 kS/s for the two channels.
**Bug fixed in this version:** the old plan sampled 3 kHz and 8 kHz at exactly 40 kframes/s, which puts the 9th/11th harmonic of the square-wave drive
exactly on the fundamental if the ADC and LEDC clocks happen to be commensurate (they are both derived from the 80 MHz APB clock). Frame rates are now
chosen per frequency by `sim/alias_search.py` so no odd harmonic up to 61 lands near the fundamental, also with +-1.5 % ADC clock error.

## What it measures / limits
Z = RCAL*rtrim*H/(G0-H) - Z0, then a weighted fit of ESR + 1/(jwC). ESR error ~ the *variation* of your clip contact resistance between calibration
and use (assumed 30 mohm). Fine for dried-out electrolytics (ESR ohms) and ESR >= ~0.3 ohm; cannot reliably tell a good from a slightly bad low-ESR
cap (ESR below ~0.1 ohm). Use short, thick leads (20 AWG, <15 cm) and firm contacts. Not measured: DC leakage, ESL, ripple capability, voltage dependence;
a bad cap in parallel with a good one cannot be separated; anything below ~200 ohm in parallel shows as PARALLEL R. Useful range about 0.47 uF to ~5 mF.

### Simulated performance (host test `test/test_dsp.cpp`, 20 runs per case, Normal accuracy, calibrated with known 4.7 ohm / 1 uF / 100 uF)
Captures are synthesised from the exact schematic model with 3 mV rms ADC noise, 30 mohm clip contact variation and +-1.2 % ADC clock error.
| Part (true C / ESR) | C measured | C error bar | ESR measured | ESR error bar |
|---|---|---|---|---|
| 0.47 uF / 0.05 ohm (film) | 0.470 | +-1.9 % | 0.01..0.08 (cal. dependent) | +-0.047 |
| 0.47 uF / 3 ohm | 0.469 | +-1.9 % | 2.95 | +-0.072 |
| 1 uF / 1 ohm | 0.999 | +-1.8 % | 0.98 | +-0.037 |
| 10 uF / 1 ohm | 9.97 | +-1.8 % | 0.99 | +-0.034 |
| 100 uF / 0.3 ohm | 99.9 | +-1.9 % | 0.299 | +-0.031 |
| 1000 uF / 0.03 ohm | 998 | +-2.1 % | 0.031 | +-0.031 |
| 4700 uF / 0.05 ohm | 4572 (-2.7 %) | +-2.5 % | 0.048 | +-0.031 |
The error bars are deliberately conservative (they include systematic allowances the simulation does not exercise). Real hardware will show
effects the simulation cannot (ADC non-linearity, supply noise, ESL, temperature, contact noise): check with known parts (below).
Time per test (estimate, unmeasured): Quick ~5 captures, Normal 10-30 captures, Precise up to ~70, each ~0.3-0.8 s including the 120 ms settling
and the soft-float maths on the C3; the open calibration takes about a minute.

## Wi-Fi and ADC noise
Wi-Fi radio bursts on the same 3.3 V rail can add ADC noise. Mitigations in the firmware: AP beacon interval ~1 s, a 32 kB ADC buffer so a capture
cannot lose samples to Wi-Fi tasks (a capture that overflows is repeated), and a "Turn Wi-Fi off until reset" button on the Good parts tab so you can compare
the noise. If your readings are noisier with Wi-Fi on, calibrate and test with it off (BOOT button) or build with `-DWIFI_ENABLED=0`.

## Status
DSP, error bars, calibration and the verdict logic (`src/capdsp.h`, `src/capjson.h`) run on the PC against synthesised captures (`test/test_dsp.cpp`):
calibration rejects wrong inputs, the error bars cover the true value in about 90-100 % of runs (2-sigma), borderline parts are flagged. The web page logic (`src/web_ui.h`) passes a fake-DOM smoke test.
The probe-sensing start (`wait_connected`) is host-tested (connect, wiggle off, reconnect, timeout, cancel). Screens (`src/screens.h`) render to PNG on the PC (`test/render_screens.cpp`).
ESP32 code (`main.cpp`, `oled.h`) compiles with ESP-IDF 5.3.1 but has **not been validated on real capacitors**. Check on real parts before trusting a verdict: short clips (ESR ~ 0), a 4.7 ohm resistor,
a few known-good and known-bad caps, an open clip, and the same part measured with Wi-Fi on and off.
Default OLED offset (before SCREEN FIT) and pins: OLED_XOFF/OLED_YOFF/OLED_SDA/OLED_SCL in platformio.ini (set -DOLED_YOFF=24 by default). Build: `pio run -t upload` (uses `partitions.csv`: the Wi-Fi build needs a 1.9 MB app partition).
Host test: `python3 test/gen_cases.py && g++ -O2 -std=c++17 -o test/test_dsp test/test_dsp.cpp && ./test/test_dsp test/cases.bin 20 1 1` (args: cases runs level(0-2) calibrate-trims(0/1); needs numpy).
