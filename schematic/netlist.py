"""Single source of truth for the analog front end.  The schematic drawing (draw_schematic.py) is checked against this
list, and the simulation (sim/chain_ac.py) is built from it.

(ref, kind, value, node1, node2)  value in ohm (R) or farad (C).  For C1 and C2 node1 is the + terminal.
Board pins: GPIO3 = drive (LEDC square wave, parked LOW when idle), GPIO0 = ADC1_CH0 (pad), GPIO4 = ADC1_CH4 (reference).
"""
COMPONENTS = [
    ("R1", "R", 330.0,  "DRV", "A"),      # drive resistor
    ("R2", "R", 10.0,   "A",   "GND"),    # shunt (1 %): sets the 97 mV p-p test level, also bleeds a charged cap
    ("C1", "C", 10e-6,  "X",   "A"),      # AC coupling, electrolytic OK: + on the ADC side (node X), 4.7-47 uF all work
    ("R5", "R", 2200.0, "X",   "ADC"),    # series: limits current into the pin
    ("R6", "R", 10e3,   "ADC", "VB"),     # bias feed
    ("R7", "R", 47e3,   "3V3", "VB"),     # bias divider top
    ("R8", "R", 10e3,   "VB",  "GND"),    # bias divider bottom  -> VB = 0.579 V
    ("C2", "C", 10e-6,  "VB",  "GND"),    # bias bypass, + on VB
    ("R3", "R", 5600.0, "DRV", "REF"),    # reference tap (drive copy / 6.6)
    ("R4", "R", 1000.0, "REF", "GND"),
]
BOARD_PINS = {"GPIO3": "DRV", "GPIO0": "ADC", "GPIO4": "REF"}   # plus the 3V3 and GND pins
POLAR = {"C1", "C2"}                       # pin order matters (node1 = +)
DUT = ("A", "GND")                   # the two clips: clip + on node A, clip - on GND
