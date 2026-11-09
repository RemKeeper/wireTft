"""Schematic symbols (KiCad lib coordinates: mm, Y up) and sheet placement."""
import parts

P = 2.54


def box(name, ref, left, right, width=15.24, pin_type="passive"):
    """left/right: lists of (number, name, electrical) or None for a gap."""
    rows = max(len(left), len(right))
    h = rows * P
    top = h / 2
    pins = []
    for k, pin in enumerate(left):
        if pin:
            pins.append(dict(num=pin[0], name=pin[1], etype=pin[2] if len(pin) > 2 else pin_type,
                             x=-width / 2 - P, y=top - P / 2 - k * P, rot=0, length=P))
    for k, pin in enumerate(right):
        if pin:
            pins.append(dict(num=pin[0], name=pin[1], etype=pin[2] if len(pin) > 2 else pin_type,
                             x=width / 2 + P, y=top - P / 2 - k * P, rot=180, length=P))
    gfx = [("rect", -width / 2, top, width / 2, -top)]
    return dict(name=name, ref=ref, pins=pins, gfx=gfx, show_pin_names=True, show_pin_numbers=True,
                ref_at=(-width / 2, top + 1.27), val_at=(-width / 2, -top - 1.27))


def sym_r():
    return dict(name="R", ref="R", show_pin_names=False, show_pin_numbers=False,
                pins=[dict(num="1", name="~", etype="passive", x=0, y=3.81, rot=270, length=1.27),
                      dict(num="2", name="~", etype="passive", x=0, y=-3.81, rot=90, length=1.27)],
                gfx=[("rect", -1.016, 2.54, 1.016, -2.54)], ref_at=(2.54, 0.635), val_at=(2.54, -1.27))


def sym_c():
    return dict(name="C", ref="C", show_pin_names=False, show_pin_numbers=False,
                pins=[dict(num="1", name="~", etype="passive", x=0, y=3.81, rot=270, length=3.048),
                      dict(num="2", name="~", etype="passive", x=0, y=-3.81, rot=90, length=3.048)],
                gfx=[("poly", [(-2.032, 0.762), (2.032, 0.762)], 0.508), ("poly", [(-2.032, -0.762), (2.032, -0.762)], 0.508)],
                ref_at=(2.54, 0.635), val_at=(2.54, -1.27))


def sym_d():
    return dict(name="D_Schottky", ref="D", show_pin_names=False, show_pin_numbers=False,
                pins=[dict(num="1", name="K", etype="passive", x=3.81, y=0, rot=180, length=2.54),
                      dict(num="2", name="A", etype="passive", x=-3.81, y=0, rot=0, length=2.54)],
                gfx=[("poly", [(-1.27, 1.27), (-1.27, -1.27), (1.27, 0), (-1.27, 1.27)], 0.254),
                     ("poly", [(0.762, 1.016), (0.762, 1.27), (1.27, 1.27), (1.27, -1.27), (1.778, -1.27), (1.778, -1.016)], 0.254)],
                ref_at=(0, 2.54), val_at=(0, -2.54))


def sym_npn():
    return dict(name="Q_NPN_BEC", ref="Q", show_pin_names=False, show_pin_numbers=True,
                pins=[dict(num="1", name="B", etype="input", x=-2.54, y=0, rot=0, length=2.54),
                      dict(num="2", name="E", etype="passive", x=2.54, y=-5.08, rot=90, length=2.54),
                      dict(num="3", name="C", etype="passive", x=2.54, y=5.08, rot=270, length=2.54)],
                gfx=[("poly", [(0, 1.905), (0, -1.905)], 0.508), ("poly", [(0, 0.635), (2.54, 2.54)], 0.254),
                     ("poly", [(0, -0.635), (2.54, -2.54)], 0.254),
                     ("poly", [(1.524, -2.286), (2.54, -2.54), (2.032, -1.524), (1.524, -2.286)], 0.254),
                     ("circle", 1.27, 0, 2.794)],
                ref_at=(5.08, 1.27), val_at=(5.08, -1.27))


def sym_nmos():
    return dict(name="Q_NMOS_GSD", ref="Q", show_pin_names=False, show_pin_numbers=True,
                pins=[dict(num="1", name="G", etype="input", x=-2.54, y=0, rot=0, length=1.905),
                      dict(num="2", name="S", etype="passive", x=2.54, y=-5.08, rot=90, length=2.54),
                      dict(num="3", name="D", etype="passive", x=2.54, y=5.08, rot=270, length=2.54)],
                gfx=[("poly", [(-0.635, 1.905), (-0.635, -1.905)], 0.254), ("poly", [(0, 2.032), (0, 1.016)], 0.254),
                     ("poly", [(0, 0.508), (0, -0.508)], 0.254), ("poly", [(0, -1.016), (0, -2.032)], 0.254),
                     ("poly", [(0, 1.524), (2.54, 1.524), (2.54, 2.54)], 0.254),
                     ("poly", [(0, -1.524), (2.54, -1.524), (2.54, -2.54)], 0.254),
                     ("poly", [(0, 0), (2.54, 0), (2.54, -1.524)], 0.254),
                     ("poly", [(0.254, 0), (1.016, 0.381), (1.016, -0.381), (0.254, 0)], 0.254),
                     ("circle", 1.27, 0, 2.794)],
                ref_at=(5.08, 1.27), val_at=(5.08, -1.27))


def sym_hole():
    return dict(name="MountingHole", ref="H", show_pin_names=False, show_pin_numbers=False, pins=[],
                gfx=[("circle", 0, 0, 1.27)], ref_at=(2.54, 0.635), val_at=(2.54, -1.27))


SYMBOLS = {
    "R": sym_r(), "C": sym_c(), "D_SCHOTTKY": sym_d(), "Q_NPN": sym_npn(), "Q_NMOS": sym_nmos(),
    "MOUNT": sym_hole(),
    "CH340C": box("CH340C", "U",
                  [("16", "VCC", "power_in"), ("4", "V3", "power_in"), ("5", "UD+", "bidirectional"),
                   ("6", "UD-", "bidirectional"), None, ("1", "GND", "power_in")],
                  [("2", "TXD", "output"), ("3", "RXD", "input"), ("13", "~{DTR}", "output"),
                   ("14", "~{RTS}", "output"), ("9", "~{CTS}", "input"), ("10", "~{DSR}", "input"),
                   ("11", "~{RI}", "input"), ("12", "~{DCD}", "input"), ("15", "R232", "input"),
                   ("7", "NC", "no_connect"), ("8", "NC", "no_connect")]),
    "USB_C": box("USB_C_Receptacle_16P", "J",
                 [("A4", "VBUS"), ("B4", "VBUS"), ("A5", "CC1"), ("B5", "CC2"), ("A6", "D+"), ("B6", "D+"),
                  ("A7", "D-"), ("B7", "D-"), ("A8", "SBU1"), ("B8", "SBU2")],
                 [("A1", "GND"), ("B1", "GND"), ("S1", "SHIELD"), ("S2", "SHIELD"), ("S3", "SHIELD"), ("S4", "SHIELD")]),
    "FPC10": box("FPC_10P_ST7789_2.4in", "FPC",
                 [(str(n), nm) for n, nm in enumerate(["GND", "3V3", "RST", "MOSI", "CLK", "CS", "DC", "LED-", "LED+", "GND", "MNT", "MNT"], 1)], [],
                 width=12.7),
    "WROOM32": box("ESP32-WROOM-32", "U",
                   [(str(k), parts.WROOM_PINS[k - 1]) for k in range(1, 20)],
                   [(str(k), parts.WROOM_PINS[k - 1]) for k in range(20, 39)], width=25.4),
    "AMS1117": box("AMS1117-3.3", "U", [("3", "VIN", "power_in"), ("1", "GND", "power_in")],
                   [("2", "VOUT", "power_out")], width=10.16),
    "SW": dict(name="SW_Push", ref="SW", show_pin_names=False, show_pin_numbers=False,
               pins=[dict(num="1", name="1", etype="passive", x=-5.08, y=0, rot=0, length=2.54),
                     dict(num="2", name="2", etype="passive", x=5.08, y=0, rot=180, length=2.54)],
               gfx=[("circle", -2.032, 0, 0.508), ("circle", 2.032, 0, 0.508),
                    ("poly", [(-2.54, 1.524), (2.54, 1.524)], 0.254), ("poly", [(0, 1.524), (0, 3.048)], 0.254)],
               ref_at=(-2.54, 4.064), val_at=(-2.54, -2.032)),
}

# Sheet placement (KiCad sheet coordinates, mm, Y down) on an A3 page
SHEET_POS = {
    "U1": (205.74, 110.49), "FPC1": (325.12, 88.9), "J1": (50.8, 191.77), "U2": (139.7, 194.31),
    "D1": (93.98, 152.4), "C1": (113.03, 152.4), "C2": (125.73, 152.4),
    "U3": (93.98, 113.03), "C10": (119.38, 113.03),
    "R4": (81.28, 220.98), "R5": (93.98, 220.98), "C3": (106.68, 233.68),
    "Q2": (205.74, 210.82), "R6": (187.96, 210.82), "Q3": (243.84, 210.82), "R7": (226.06, 210.82),
    "C4": (349.25, 133.35), "C5": (361.95, 133.35), "R1": (304.8, 152.4), "Q1": (317.5, 175.26),
    "R2": (292.1, 175.26), "R3": (309.88, 189.23),
    "C6": (266.7, 59.69), "C7": (279.4, 59.69), "R8": (138.43, 66.04), "C9": (151.13, 66.04),
    "SW1": (139.7, 81.28), "R9": (266.7, 144.78), "SW2": (279.4, 160.02),
    "H1": (360.68, 228.6), "H2": (373.38, 228.6), "H3": (360.68, 241.3), "H4": (373.38, 241.3),
}
