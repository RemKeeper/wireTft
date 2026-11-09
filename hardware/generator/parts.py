"""Board data: footprints, components, nets and placement (board coordinates, mm, Y down, front view).

Components with side="B" are mounted on the back; their footprints are defined top-view and mirrored in X.
"""

BOARD_W, BOARD_H = 56.0, 90.0
CX = 28.0  # board centre line

# Front-side area reserved for the 2.4" panel (glass + backlight); only traces and vias inside.
SCREEN = (6.0, 8.0, 50.0, 69.0)

# ESP32-WROOM-32 on the back, antenna end flush with the top edge.
MOD_Y = 15.74            # footprint origin; module outline spans local y -15.74 .. 9.76
MOUNT_HOLES = [(3.2, 3.2), (BOARD_W - 3.2, 3.2), (3.2, BOARD_H - 3.2), (BOARD_W - 3.2, BOARD_H - 3.2)]

FPC_Y = 72.0
USB_Y = BOARD_H - 3.3


def pad(num, kind, shape, x, y, w, h, drill=None, layers=None, oval_drill=None, angle=0):
    if layers is None:
        layers = ("F.Cu", "F.Paste", "F.Mask") if kind == "smd" else ("*.Cu", "*.Mask")
    return dict(num=num, kind=kind, shape=shape, x=x, y=y, w=w, h=h, drill=drill,
                oval_drill=oval_drill, layers=layers, angle=angle)


def line(x1, y1, x2, y2, layer="F.SilkS", width=0.12):
    return ("line", x1, y1, x2, y2, layer, width)


def rect_lines(x1, y1, x2, y2, layer="F.SilkS", width=0.12):
    return [line(x1, y1, x2, y1, layer, width), line(x2, y1, x2, y2, layer, width),
            line(x2, y2, x1, y2, layer, width), line(x1, y2, x1, y1, layer, width)]


def circle(cx, cy, r, layer="F.SilkS", width=0.12):
    return ("circle", cx, cy, r, layer, width)


def fp_r0603(vertical=False):
    if vertical:
        pads = [pad("1", "smd", "roundrect", 0, -0.825, 0.95, 0.8), pad("2", "smd", "roundrect", 0, 0.825, 0.95, 0.8)]
        gfx = [line(-0.8, -0.2, -0.8, 0.2), line(0.8, -0.2, 0.8, 0.2)]
        return dict(name="R_0603_V", pads=pads, gfx=gfx, ref_at=(1.6, 0), attr="smd")
    pads = [pad("1", "smd", "roundrect", -0.825, 0, 0.8, 0.95), pad("2", "smd", "roundrect", 0.825, 0, 0.8, 0.95)]
    gfx = [line(-0.2, -0.8, 0.2, -0.8), line(-0.2, 0.8, 0.2, 0.8)]
    return dict(name="R_0603", pads=pads, gfx=gfx, ref_at=(0, -1.4), attr="smd")


def fp_sot23():
    pads = [pad("1", "smd", "roundrect", -0.9375, -0.95, 1.475, 0.6),
            pad("2", "smd", "roundrect", -0.9375, 0.95, 1.475, 0.6),
            pad("3", "smd", "roundrect", 0.9375, 0, 1.475, 0.6)]
    gfx = [line(-0.76, -1.56, 0.76, -1.56), line(-0.76, 1.56, 0.76, 1.56),
           line(0.76, -1.56, 0.76, -0.6), line(0.76, 0.6, 0.76, 1.56), circle(-1.9, -1.5, 0.1)]
    return dict(name="SOT-23", pads=pads, gfx=gfx, ref_at=(0, -2.4), attr="smd")


def fp_sot223():
    # AMS1117: 1 GND, 2 VOUT (tab), 3 VIN
    pads = [pad("1", "smd", "roundrect", -3.15, -2.3, 2.0, 1.5), pad("2", "smd", "roundrect", -3.15, 0, 2.0, 1.5),
            pad("3", "smd", "roundrect", -3.15, 2.3, 2.0, 1.5), pad("2", "smd", "roundrect", 3.15, 0, 2.0, 3.8)]
    gfx = [line(-1.85, -3.41, 1.91, -3.41), line(-1.85, 3.41, 1.91, 3.41), line(1.91, -3.41, 1.91, -2.15),
           line(1.91, 3.41, 1.91, 2.15), circle(-4.6, -2.3, 0.15)]
    return dict(name="SOT-223-3_TabPin2", pads=pads, gfx=gfx, ref_at=(0, -4.4), attr="smd")


def fp_sod123_rev():
    # anode (pin 2) on the left, cathode (pin 1) on the right
    pads = [pad("1", "smd", "roundrect", 1.65, 0, 0.9, 1.2), pad("2", "smd", "roundrect", -1.65, 0, 0.9, 1.2)]
    gfx = [line(-1.0, -0.95, 1.0, -0.95), line(-1.0, 0.95, 1.0, 0.95), line(0.6, -0.95, 0.6, 0.95),
           line(2.35, -0.9, 2.35, 0.9)]
    return dict(name="D_SOD-123_KR", pads=pads, gfx=gfx, ref_at=(0, -1.8), attr="smd")


def fp_soic16_h():
    # SOIC-16 rotated 90 deg CCW: pins 1..8 bottom row left->right, 9..16 top row right->left
    pads = []
    for k in range(1, 9):
        pads.append(pad(str(k), "smd", "roundrect", -4.445 + 1.27 * (k - 1), 2.475, 0.6, 1.95))
    for m in range(1, 9):
        pads.append(pad(str(8 + m), "smd", "roundrect", 4.445 - 1.27 * (m - 1), -2.475, 0.6, 1.95))
    gfx = rect_lines(-5.06, -1.56, 5.06, 1.56) + [circle(-5.4, 2.9, 0.15)]
    return dict(name="SOIC-16_H", pads=pads, gfx=gfx, ref_at=(0, 0), attr="smd")


def fp_fpc10():
    # Same land pattern as FPC1 (RH24099) on the original adapter board, rotated 180 deg so the
    # opening faces the panel (north). Signal pads on the opening side.
    pads = [pad(str(n), "smd", "rect", -2.25 + 0.5 * (n - 1), -1.25, 0.3, 0.8) for n in range(1, 11)]
    pads.append(pad("11", "smd", "rect", 3.25, 1.25, 0.4, 0.8))
    pads.append(pad("12", "smd", "rect", -3.25, 1.25, 0.4, 0.8))
    gfx = [line(-3.5, 1.905, 3.5, 1.905), line(-3.5, -1.1, -3.5, 0.625), line(3.5, -1.1, 3.5, 0.625),
           line(-3.5, -1.1, -2.63, -1.1), line(3.5, -1.1, 2.63, -1.1), circle(-2.25, -2.0, 0.12)]
    return dict(name="FPC_RH24099_10P_P0.5", pads=pads, gfx=gfx, ref_at=(0, 3.0), attr="smd")


def fp_usbc():
    pads = []
    for num, x, w in [("A1", -3.25, 0.6), ("A4", -2.45, 0.6), ("B8", -1.75, 0.3), ("A5", -1.25, 0.3),
                      ("B7", -0.75, 0.3), ("A6", -0.25, 0.3), ("A7", 0.25, 0.3), ("B6", 0.75, 0.3),
                      ("A8", 1.25, 0.3), ("B5", 1.75, 0.3), ("B4", 2.45, 0.6), ("B1", 3.25, 0.6)]:
        pads.append(pad(num, "smd", "roundrect", x, -4.045, w, 1.45))
    pads.append(pad("", "np_thru_hole", "circle", -2.89, -2.6, 0.65, 0.65, drill=0.65, layers=("*.Cu", "*.Mask")))
    pads.append(pad("", "np_thru_hole", "circle", 2.89, -2.6, 0.65, 0.65, drill=0.65, layers=("*.Cu", "*.Mask")))
    # slots written as horizontal ovals rotated 90 so every consumer agrees on the slot direction
    pads.append(pad("S1", "thru_hole", "oval", -4.32, -3.13, 2.1, 1.0, oval_drill=(1.7, 0.6), angle=90))
    pads.append(pad("S2", "thru_hole", "oval", 4.32, -3.13, 2.1, 1.0, oval_drill=(1.7, 0.6), angle=90))
    pads.append(pad("S3", "thru_hole", "oval", -4.32, 1.05, 1.6, 1.0, oval_drill=(1.2, 0.6), angle=90))
    pads.append(pad("S4", "thru_hole", "oval", 4.32, 1.05, 1.6, 1.0, oval_drill=(1.2, 0.6), angle=90))
    gfx = [line(-4.7, -1.9, -4.7, 0.1), line(-4.7, 2.0, -4.7, 3.3), line(4.7, -1.9, 4.7, 0.1),
           line(4.7, 2.0, 4.7, 3.3)] + rect_lines(-4.47, -3.65, 4.47, 3.65, "F.Fab", 0.1)
    return dict(name="USB_C_HRO_TYPE-C-31-M-12", pads=pads, gfx=gfx, ref_at=(0, -6.2), attr="smd")


def fp_wroom32():
    # KiCad RF_Module:ESP32-WROOM-32 land pattern (top view); bottom-row pads pre-rotated, exposed pad omitted.
    pads = [pad(str(k), "smd", "rect", -8.75, -8.25 + 1.27 * (k - 1), 1.5, 0.9) for k in range(1, 15)]
    pads += [pad(str(k), "smd", "rect", -5.71 + 1.27 * (k - 15), 9.51, 0.9, 1.5) for k in range(15, 25)]
    pads += [pad(str(k), "smd", "rect", 8.75, 8.26 - 1.27 * (k - 25), 1.5, 0.9) for k in range(25, 39)]
    gfx = [line(-9.12, -15.86, 9.12, -15.86), line(-9.12, -15.86, -9.12, -9.7), line(9.12, -15.86, 9.12, -9.445),
           line(-9.12, 9.1, -9.12, 9.88), line(-9.12, 9.88, -8.12, 9.88), line(9.12, 9.1, 9.12, 9.88),
           line(9.12, 9.88, 8.12, 9.88), line(-9.12, -9.8, 9.12, -9.8, "F.SilkS", 0.08), circle(-9.6, -8.25, 0.2)]
    return dict(name="ESP32-WROOM-32", pads=pads, gfx=gfx, ref_at=(0, 3.0), attr="smd")


def fp_ts1187a():
    pads = [pad("1", "smd", "rect", -3.0, -1.875, 1.0, 0.75), pad("1", "smd", "rect", 3.0, -1.875, 1.0, 0.75),
            pad("2", "smd", "rect", -3.0, 1.875, 1.0, 0.75), pad("2", "smd", "rect", 3.0, 1.875, 1.0, 0.75)]
    gfx = [line(-2.75, -1.0, -2.75, 1.0), line(2.75, -1.0, 2.75, 1.0), line(-1.3, -2.75, 1.3, -2.75),
           line(-1.3, 2.75, 1.3, 2.75), line(-1.75, -2.3, -1.3, -2.75), line(1.75, -2.3, 1.3, -2.75),
           line(-1.75, 2.3, -1.3, 2.75), line(1.75, 2.3, 1.3, 2.75), circle(0, 0, 1.0)]
    return dict(name="SW_Push_TS-1187A", pads=pads, gfx=gfx, ref_at=(0, -3.6), attr="smd")


def fp_mount_hole():
    pads = [pad("", "np_thru_hole", "circle", 0, 0, 3.2, 3.2, drill=3.2, layers=("*.Cu", "*.Mask"))]
    return dict(name="MountingHole_3.2mm_M3", pads=pads, gfx=[circle(0, 0, 2.9, "F.SilkS", 0.12)], ref_at=(0, -4.0), attr="exclude", hide_ref=True)


FOOTPRINTS = {f["name"]: f for f in [fp_r0603(), fp_r0603(True), fp_sot23(), fp_sot223(), fp_sod123_rev(), fp_soic16_h(),
                                       fp_fpc10(), fp_usbc(), fp_wroom32(), fp_ts1187a(), fp_mount_hole()]}

WROOM_PINS = ["GND", "3V3", "EN", "SENSOR_VP", "SENSOR_VN", "IO34", "IO35", "IO32", "IO33", "IO25", "IO26", "IO27",
              "IO14", "IO12", "GND", "IO13", "SD2", "SD3", "CMD", "CLK", "SD0", "SD1", "IO15", "IO2", "IO0", "IO4",
              "IO16", "IO17", "IO5", "IO18", "IO19", "NC", "IO21", "RXD0", "TXD0", "IO22", "IO23", "GND"]


def wp(name):
    return str(WROOM_PINS.index(name) + 1)


SY = 6.0  # the front component strip sits 6 mm lower than in the DevKitC revision

# Component: ref, symbol, value, footprint, position, pad->net, LCSC, description, side
COMPONENTS = [
    dict(ref="FPC1", sym="FPC10", value="FPC 0.5-10P HYH2.0", fp="FPC_RH24099_10P_P0.5", at=(CX + 0.05, FPC_Y),
         lcsc="C6364659", desc="SHOU HAN 0.5mm 10P flip-top, double-sided (top+bottom) contacts, H2.0; "
                               "panel FPC folds behind the screen, pin order mirrored",
         nets={"1": "GND", "2": "+3V3", "3": "LCD_RST", "4": "LCD_MOSI", "5": "LCD_CLK", "6": "LCD_CS",
               "7": "LCD_DC", "8": "LED-", "9": "LED+", "10": "GND", "11": "GND", "12": "GND"}),
    dict(ref="J1", sym="USB_C", value="TYPE-C-31-M-12", fp="USB_C_HRO_TYPE-C-31-M-12", at=(CX + 0.05, USB_Y),
         lcsc="C165948", desc="USB Type-C 16P receptacle",
         nets={"A1": "GND", "B1": "GND", "A4": "VBUS", "B4": "VBUS", "A5": "CC1", "B5": "CC2",
               "A6": "USB_DP", "B6": "USB_DP", "A7": "USB_DM", "B7": "USB_DM",
               "S1": "GND", "S2": "GND", "S3": "GND", "S4": "GND"}),
    dict(ref="U2", sym="CH340C", value="CH340C", fp="SOIC-16_H", at=(13.0, 75.0 + SY), lcsc="C84681",
         desc="USB-UART bridge, internal oscillator, runs from 3.3 V",
         nets={"1": "GND", "2": "U0RXD", "3": "U0TXD", "4": "+3V3", "5": "USB_DP", "6": "USB_DM",
               "13": "DTR", "14": "RTS", "16": "+3V3"}),
    dict(ref="D1", sym="D_SCHOTTKY", value="B5819W", fp="D_SOD-123_KR", at=(38.6, 77.2 + SY), lcsc="C8598",
         desc="Reverse protection VBUS -> 5V", nets={"1": "+5V", "2": "VBUS"}),
    dict(ref="R4", sym="R", value="5.1k", fp="R_0603_V", at=(22.4, 76.4 + SY), lcsc="C23186", desc="CC1 pull-down",
         nets={"1": "CC1", "2": "GND"}),
    dict(ref="R5", sym="R", value="5.1k", fp="R_0603", at=(34.0, 74.3 + SY), lcsc="C23186", desc="CC2 pull-down",
         nets={"1": "CC2", "2": "GND"}),
    dict(ref="C1", sym="C", value="10uF", fp="R_0603_V", at=(43.6, 77.2 + SY), lcsc="C19702", desc="5V bulk / LDO input",
         nets={"1": "+5V", "2": "GND"}),
    dict(ref="C2", sym="C", value="100nF", fp="R_0603_V", at=(46.2, 77.2 + SY), lcsc="C14663", desc="5V decoupling",
         nets={"1": "+5V", "2": "GND"}),
    dict(ref="U3", sym="AMS1117", value="AMS1117-3.3", fp="SOT-223-3_TabPin2", at=(50.6, 74.4), lcsc="C6186",
         desc="5V -> 3.3V LDO, 1A", nets={"1": "GND", "2": "+3V3", "3": "+5V"}),
    dict(ref="C10", sym="C", value="10uF", fp="R_0603_V", at=(49.6, 80.2), lcsc="C19702", desc="LDO output",
         nets={"1": "+3V3", "2": "GND"}),
    dict(ref="C3", sym="C", value="100nF", fp="R_0603_V", at=(5.6, 74.4 + SY), lcsc="C14663", desc="CH340C VCC/V3",
         nets={"1": "+3V3", "2": "GND"}),
    dict(ref="Q2", sym="Q_NPN", value="MMBT3904", fp="SOT-23", at=(4.6, 66.6 + SY), lcsc="C20526",
         desc="Auto-download: DTR/RTS -> EN", nets={"1": "Q2_B", "2": "RTS", "3": "EN"}),
    dict(ref="R6", sym="R", value="10k", fp="R_0603", at=(8.6, 66.6 + SY), lcsc="C25804", desc="Q2 base",
         nets={"1": "DTR", "2": "Q2_B"}),
    dict(ref="Q3", sym="Q_NPN", value="MMBT3904", fp="SOT-23", at=(12.4, 66.6 + SY), lcsc="C20526",
         desc="Auto-download: DTR/RTS -> IO0", nets={"1": "Q3_B", "2": "DTR", "3": "IO0"}),
    dict(ref="R7", sym="R", value="10k", fp="R_0603", at=(16.4, 66.6 + SY), lcsc="C25804", desc="Q3 base",
         nets={"1": "RTS", "2": "Q3_B"}),
    dict(ref="C5", sym="C", value="100nF", fp="R_0603_V", at=(19.6, 66.6 + SY), lcsc="C14663", desc="Panel 3V3",
         nets={"1": "+3V3", "2": "GND"}),
    dict(ref="C4", sym="C", value="10uF", fp="R_0603_V", at=(21.8, 66.6 + SY), lcsc="C19702", desc="Panel 3V3 bulk",
         nets={"1": "+3V3", "2": "GND"}),
    dict(ref="R1", sym="R", value="10R", fp="R_0603", at=(35.4, 65.2 + SY), lcsc="C22859", desc="Backlight current limit",
         nets={"1": "+3V3", "2": "LED+"}),
    dict(ref="Q1", sym="Q_NMOS", value="SI2302", fp="SOT-23", at=(39.4, 67.6 + SY), lcsc="C10487",
         desc="Backlight low-side switch", nets={"1": "Q1_G", "2": "GND", "3": "LED-"}),
    dict(ref="R2", sym="R", value="1k", fp="R_0603", at=(43.2, 65.6 + SY), lcsc="C21190", desc="Q1 gate series",
         nets={"1": "LCD_BLK", "2": "Q1_G"}),
    dict(ref="R3", sym="R", value="10k", fp="R_0603_V", at=(42.6, 69.6 + SY), lcsc="C25804", desc="Q1 gate pull-down",
         nets={"1": "Q1_G", "2": "GND"}),
    # ---- back side
    dict(ref="U1", sym="WROOM32", value="ESP32-WROOM-32", fp="ESP32-WROOM-32", at=(CX, MOD_Y), side="B",
         lcsc="C82899", desc="ESP32-WROOM-32 (4MB), soldered on the back, antenna at the top edge",
         nets={"1": "GND", "15": "GND", "38": "GND", wp("3V3"): "+3V3", wp("EN"): "EN", wp("IO32"): "LCD_BLK",
               wp("IO33"): "LCD_RST", wp("IO27"): "LCD_DC", wp("IO0"): "IO0", wp("IO18"): "LCD_CLK",
               wp("IO21"): "LCD_CS", wp("RXD0"): "U0RXD", wp("TXD0"): "U0TXD", wp("IO23"): "LCD_MOSI"}),
    dict(ref="C6", sym="C", value="10uF", fp="R_0603", at=(40.0, 7.6), side="B", lcsc="C19702", desc="Module 3V3 bulk",
         nets={"1": "+3V3", "2": "GND"}),
    dict(ref="C7", sym="C", value="100nF", fp="R_0603", at=(40.0, 9.6), side="B", lcsc="C14663", desc="Module 3V3 decoupling",
         nets={"1": "+3V3", "2": "GND"}),
    dict(ref="R8", sym="R", value="10k", fp="R_0603", at=(40.0, 11.6), side="B", lcsc="C25804", desc="EN pull-up",
         nets={"1": "EN", "2": "+3V3"}),
    dict(ref="C9", sym="C", value="1uF", fp="R_0603", at=(40.0, 13.6), side="B", lcsc="C15849", desc="EN reset delay",
         nets={"1": "EN", "2": "GND"}),
    dict(ref="SW1", sym="SW", value="RST", fp="SW_Push_TS-1187A", at=(47.0, 13.0), side="B", lcsc="C318884",
         desc="Reset button (EN)", nets={"1": "EN", "2": "GND"}),
    dict(ref="R9", sym="R", value="10k", fp="R_0603", at=(15.6, 27.6), side="B", lcsc="C25804", desc="IO0 pull-up",
         nets={"1": "+3V3", "2": "IO0"}),
    dict(ref="SW2", sym="SW", value="BOOT", fp="SW_Push_TS-1187A", at=(9.6, 24.0), side="B", lcsc="C318884",
         desc="Boot button (IO0)", nets={"1": "IO0", "2": "GND"}),
]
for i, (hx, hy) in enumerate(MOUNT_HOLES, 1):
    COMPONENTS.append(dict(ref=f"H{i}", sym="MOUNT", value="M3", fp="MountingHole_3.2mm_M3", at=(hx, hy),
                           lcsc="", desc="M3 mounting hole", nets={}, no_bom=True))

# Track widths per net (mm); everything else uses SIGNAL_W.
SIGNAL_W = 0.25
NET_W = {"GND": 0.4, "+3V3": 0.5, "+5V": 0.6, "VBUS": 0.6, "LED+": 0.3, "LED-": 0.3}
# Pads whose pitch caps the width of a connection that ends on them.
FINE_PAD_W = {("FPC1", n): 0.25 for n in [str(i) for i in range(1, 11)]}
FINE_PAD_W.update({("J1", n): 0.25 for n in ["A5", "A6", "A7", "B5", "B6", "B7"]})
# Hand-made escapes for the two 0.5 mm pitch connectors: (net, layer, points, width) and (net, x, y).
_U = CX + 0.05
_PY = USB_Y - 4.045   # USB-C signal pad centre row
_VY = _PY - 1.5       # VBUS jumper vias
_R4 = COMPONENTS[4]["at"]
_R5 = COMPONENTS[5]["at"]
FIXED_TRACKS = [
    ("VBUS", 0, [(_U - 2.45, _PY), (_U - 2.45, _VY)], 0.6),
    ("VBUS", 1, [(_U - 2.45, _VY), (_U + 2.45, _VY)], 0.6),
    ("VBUS", 0, [(_U + 2.45, _PY), (_U + 2.45, _VY)], 0.6),
    ("CC1", 0, [(_U - 1.25, _PY), (_U - 1.25, _PY - 2.305), (_R4[0] + 0.5, _PY - 2.305), (_R4[0], _PY - 1.805),
                (_R4[0], _R4[1] - 0.825)], 0.25),
    ("USB_DM", 0, [(_U - 0.75, _PY), (_U - 0.75, _PY - 2.805), (24.5, _PY - 2.805)], 0.25),
    ("USB_DP", 0, [(_U - 0.25, _PY), (_U - 0.25, _PY - 3.305), (24.5, _PY - 3.305)], 0.25),
    ("USB_DP", 0, [(_U + 0.75, _PY), (_U + 0.75, _PY - 1.055), (_U + 1.25, _PY - 1.555), (_U + 1.25, _PY - 3.805),
                   (_U - 0.25, _PY - 3.805), (_U - 0.25, _PY - 3.305)], 0.25),
    ("USB_DM", 0, [(_U + 0.25, _PY), (_U + 0.25, _PY - 2.055), (_U + 0.5, _PY - 2.305), (_U + 0.5, _PY - 2.655)], 0.25),
    ("CC2", 0, [(_U + 1.75, _PY), (_U + 1.75, _R5[1]), (_R5[0] - 0.825, _R5[1])], 0.25),
    ("LCD_RST", 0, [(_U - 1.25, FPC_Y - 1.25), (_U - 1.25, FPC_Y - 2.4), (_U - 1.55, FPC_Y - 2.7)], 0.25),
]
FIXED_VIAS = [("VBUS", _U - 2.45, _VY), ("VBUS", _U + 2.45, _VY), ("USB_DM", _U + 0.5, _PY - 2.655),
              ("LCD_RST", _U - 1.55, FPC_Y - 2.7)]

CLEARANCE = 0.2
EDGE_CLEARANCE = 0.3
VIA_D, VIA_DRILL = 0.6, 0.3
POWER_VIA_D, POWER_VIA_DRILL = 0.8, 0.4
POWER_NETS = {"+5V", "VBUS", "GND", "+3V3"}

# Copper keep-outs: (x1, y1, x2, y2, layers)
ANTENNA_KEEPOUT = (CX - 12.0, 0.0, CX + 12.0, MOD_Y - 9.3, ("F.Cu", "B.Cu"))
KEEPOUTS = [
    ANTENNA_KEEPOUT,                                                   # WROOM-32 PCB antenna
    (CX - 7.7, 0.0, CX + 7.7, MOD_Y + 8.4, ("B.Cu",)),                 # under the module body (back)
    (CX + 0.05 - 2.65, FPC_Y - 0.7, CX + 0.05 + 2.65, FPC_Y + 1.7, ("F.Cu",)),   # under FPC connector body
    (CX + 0.05 - 3.55, USB_Y - 2.9, CX + 0.05 + 3.55, BOARD_H, ("F.Cu",)),        # under USB-C shell
]
