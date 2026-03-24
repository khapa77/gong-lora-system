#!/usr/bin/env python3
"""Генератор EasyEDA Standard JSON схемы для Gong LoRa Server."""
import json

counter = [0]
def uid():
    counter[0] += 1
    return f'g{counter[0]:05d}'

shapes = []

def rect(x, y, w, h, fill='#eef0ff', stroke='#000000'):
    # EasyEDA Standard: RECT~x~y~rx~ry~width~height~stroke~fill~strokeWidth~strokeStyle~id~locked
    shapes.append(f'RECT~{x}~{y}~0~0~{w}~{h}~{stroke}~{fill}~1~none~{uid()}~0')

def text(x, y, s, anchor='center', size='9pt', bold='normal', color='#000000'):
    s = str(s).replace('~', '-')
    # EasyEDA Standard: T~x~y~rotation~anchor~color~fontSize~bold~italic~underline~text~id~locked
    shapes.append(f'T~{x}~{y}~0~{anchor}~{color}~{size}~{bold}~normal~none~{s}~{uid()}~0')

def wire(x1, y1, x2, y2):
    # EasyEDA Standard: W~x1 y1 x2 y2~color~strokeStyle~strokeWidth~fillColor~id~locked
    shapes.append(f'W~{x1} {y1} {x2} {y2}~#000000~none~1~none~{uid()}~0')

def netlabel(x, y, name, rot=0):
    # EasyEDA Standard: N~x~y~rotation~text~color~fontSize~fontStyle~fontWeight~id~locked
    shapes.append(f'N~{x}~{y}~{rot}~{name}~#0000FF~9pt~normal~normal~{uid()}~0')

def nc_mark(x, y):
    """Крестик NC на конце провода."""
    wire(x-5, y-5, x+5, y+5)
    wire(x-5, y+5, x+5, y-5)

# ─────────────────────────────────────────
# TITLE
# ─────────────────────────────────────────
text(430, 22, 'Gong LoRa Server — Schematic v1.0',
     anchor='center', size='13pt', bold='bold', color='#222222')

# ─────────────────────────────────────────
# U1 — ESP32 DevKitC v4
# ─────────────────────────────────────────
BX, BY, BW, BH = 310, 60, 160, 440   # body rect
rect(BX, BY, BW, BH)
text(BX+BW//2, BY+14, 'U1', bold='bold', size='11pt')
text(BX+BW//2, BY+30, 'ESP32-DevKitC-V4', size='8pt')

STUB = 30   # длина вывода
PIN_PITCH = 20
PIN_START = BY + 40

# Left pins (top→bottom)
left_pins = [
    'GND', '3V3', 'EN', 'GPIO36', 'GPIO39',
    'GPIO34', 'GPIO35', 'GPIO32',
    'GPIO33',   # I2S_DIN
    'GPIO25',   # I2S_LRC
    'GPIO26',   # I2S_BCLK
    'GPIO27',
    'GPIO14',   # LORA_RST
    'GPIO12', 'GND', 'GPIO13',
    'GPIO9', 'GPIO10', 'GPIO11',
]
left_nets = {
    'GND':    'GND',
    '3V3':    '+3V3',
    'GPIO33': 'I2S_DIN',
    'GPIO25': 'I2S_LRC',
    'GPIO26': 'I2S_BCLK',
    'GPIO14': 'LORA_RST',
}
seen_gnd_left = 0
for i, pname in enumerate(left_pins):
    py = PIN_START + i * PIN_PITCH
    wx = BX - STUB
    wire(wx, py, BX, py)
    text(BX + 6, py, pname, anchor='left', size='7pt', color='#444444')
    if pname == 'GND':
        seen_gnd_left += 1
        netlabel(wx, py, 'GND', rot=180)
    elif pname in left_nets:
        netlabel(wx, py, left_nets[pname], rot=180)
    else:
        nc_mark(wx, py)

# Right pins (top→bottom)
right_pins = [
    'VIN',      # +5V
    'GND',
    'GPIO23',   # LORA_MOSI
    'GPIO22',   # I2C_SCL
    'GPIO1', 'GPIO3',
    'GPIO21',   # I2C_SDA
    'GND',
    'GPIO19',   # LORA_MISO
    'GPIO18',   # LORA_SCK
    'GPIO5',    # LORA_NSS
    'GPIO17', 'GPIO16', 'GPIO4', 'GPIO0',
    'GPIO2',    # LORA_DIO0
    'GPIO15', 'GPIO8', 'GPIO7',
]
right_nets = {
    'VIN':    '+5V',
    'GND':    'GND',
    'GPIO23': 'LORA_MOSI',
    'GPIO22': 'I2C_SCL',
    'GPIO21': 'I2C_SDA',
    'GPIO19': 'LORA_MISO',
    'GPIO18': 'LORA_SCK',
    'GPIO5':  'LORA_NSS',
    'GPIO2':  'LORA_DIO0',
}
for i, pname in enumerate(right_pins):
    py = PIN_START + i * PIN_PITCH
    ex = BX + BW + STUB
    wire(BX + BW, py, ex, py)
    text(BX + BW - 6, py, pname, anchor='right', size='7pt', color='#444444')
    if pname in right_nets:
        netlabel(ex, py, right_nets[pname], rot=0)
    else:
        nc_mark(ex, py)

# ─────────────────────────────────────────
# U2 — Ra-01 LoRa  (2×4, справа)
# ─────────────────────────────────────────
R2X, R2Y = 590, 60
rect(R2X, R2Y, 130, 110)
text(R2X+65, R2Y+14, 'U2', bold='bold', size='11pt')
text(R2X+65, R2Y+28, 'Ra-01  SX1278', size='8pt')

# Left col: pin1 MISO, pin3 SCK, pin5 NSS, pin7 RST
ra_left = [('MISO','LORA_MISO'), ('SCK','LORA_SCK'),
           ('NSS','LORA_NSS'),   ('RST','LORA_RST')]
for i,(pn,net) in enumerate(ra_left):
    py = R2Y + 42 + i*20
    wire(R2X-STUB, py, R2X, py)
    text(R2X+5, py, pn, anchor='left', size='7pt', color='#444444')
    netlabel(R2X-STUB, py, net, rot=180)

# Right col: pin2 VCC, pin4 MOSI, pin6 DIO0, pin8 GND
ra_right = [('VCC','+3V3'), ('MOSI','LORA_MOSI'),
            ('DIO0','LORA_DIO0'), ('GND','GND')]
for i,(pn,net) in enumerate(ra_right):
    py = R2Y + 42 + i*20
    ex = R2X + 130 + STUB
    wire(R2X+130, py, ex, py)
    text(R2X+125, py, pn, anchor='right', size='7pt', color='#444444')
    netlabel(ex, py, net, rot=0)

# ─────────────────────────────────────────
# U3 — MAX98357A (1×7, справа)
# ─────────────────────────────────────────
U3X, U3Y = 590, 230
rect(U3X, U3Y, 130, 170)
text(U3X+65, U3Y+14, 'U3', bold='bold', size='11pt')
text(U3X+65, U3Y+28, 'MAX98357A', size='8pt')
text(U3X+65, U3Y+42, 'I2S Amp', size='7pt', color='#666666')

u3_pins = [
    ('LRC',  'I2S_LRC'),
    ('BCLK', 'I2S_BCLK'),
    ('DIN',  'I2S_DIN'),
    ('GAIN', None),        # NC
    ('SD',   'MAX_SD'),
    ('GND',  'GND'),
    ('VIN',  '+5V'),
]
for i,(pn,net) in enumerate(u3_pins):
    py = U3Y + 52 + i*20
    wire(U3X-STUB, py, U3X, py)
    text(U3X+5, py, pn, anchor='left', size='7pt', color='#444444')
    if net:
        netlabel(U3X-STUB, py, net, rot=180)
    else:
        text(U3X-STUB-8, py, 'NC', anchor='right', size='7pt', color='#999999')

# ─────────────────────────────────────────
# U4 — DS3231 RTC ZS-042 (1×4, справа)
# ─────────────────────────────────────────
U4X, U4Y = 590, 460
rect(U4X, U4Y, 130, 100)
text(U4X+65, U4Y+14, 'U4', bold='bold', size='11pt')
text(U4X+65, U4Y+28, 'DS3231 ZS-042', size='8pt')
text(U4X+65, U4Y+42, 'RTC  I2C', size='7pt', color='#666666')

u4_pins = [
    ('GND', 'GND'),
    ('VCC', '+3V3'),
    ('SDA', 'I2C_SDA'),
    ('SCL', 'I2C_SCL'),
]
for i,(pn,net) in enumerate(u4_pins):
    py = U4Y + 52 + i*20
    wire(U4X-STUB, py, U4X, py)
    text(U4X+5, py, pn, anchor='left', size='7pt', color='#444444')
    netlabel(U4X-STUB, py, net, rot=180)

# ─────────────────────────────────────────
# PS1 — HLK-10M05
# ─────────────────────────────────────────
PSX, PSY = 60, 80
rect(PSX, PSY, 160, 110)
text(PSX+80, PSY+14, 'PS1', bold='bold', size='11pt')
text(PSX+80, PSY+28, 'HLK-10M05', size='8pt')
text(PSX+80, PSY+42, 'AC-DC 5V / 2A', size='7pt', color='#666666')
text(PSX+80, PSY+58, '!!! FUSE 1A on L !!!', size='7pt', color='#cc0000', bold='bold')

# AC in (left)
for i,(pn,net) in enumerate([('L','AC_L'), ('N','AC_N')]):
    py = PSY + 72 + i*20
    wire(PSX-STUB, py, PSX, py)
    text(PSX+5, py, pn, anchor='left', size='7pt', color='#444444')
    netlabel(PSX-STUB, py, net, rot=180)

# DC out (right)
for i,(pn,net) in enumerate([('+Vo','+5V'), ('-Vo','GND')]):
    py = PSY + 72 + i*20
    ex = PSX + 160 + STUB
    wire(PSX+160, py, ex, py)
    text(PSX+154, py, pn, anchor='right', size='7pt', color='#444444')
    netlabel(ex, py, net, rot=0)

# ─────────────────────────────────────────
# Пассивные компоненты
# ─────────────────────────────────────────
def passive(x, y, ref, value, net_top, net_bot, fill='#fff8e8'):
    rect(x, y, 30, 50, fill=fill)
    text(x+15, y+25, ref, size='7pt', bold='bold')
    text(x+35, y+20, value, anchor='left', size='7pt', color='#333333')
    # top pin
    wire(x+15, y-15, x+15, y)
    netlabel(x+15, y-15, net_top, rot=270)  # вверх
    # bottom pin
    wire(x+15, y+50, x+15, y+65)
    netlabel(x+15, y+65, net_bot, rot=90)   # вниз

passive(70,  310, 'C1', '100uF/10V', '+5V',  'GND')
passive(120, 310, 'C2', '100nF',     '+5V',  'GND')
passive(70,  420, 'C3', '100nF',     '+3V3', 'GND')
passive(70,  530, 'R1', '4.7k',      '+3V3', 'I2C_SDA', fill='#fff0f0')
passive(120, 530, 'R2', '4.7k',      '+3V3', 'I2C_SCL', fill='#fff0f0')
passive(70,  640, 'R3', '1M',        '+5V',  'MAX_SD',  fill='#fff0f0')

# ─────────────────────────────────────────
# BUILD JSON
# ─────────────────────────────────────────
CANVAS = "CA~1000~1000~#ffffff~yes~#cccccc~10~1200~900~line~10~pixel~5~0~0"
schematic = {
    "head": {
        "type": "schematic",
        "title": "Gong LoRa Server",
        "description": "ESP32-DevKitC-V4 + Ra-01 LoRa + MAX98357A + DS3231 + HLK-10M05",
        "canvas": CANVAS,
        "version": "6.5.38",
        "encryptedDataCompliant": False
    },
    "canvas": CANVAS,
    "shape": shapes,
    "BBox": None,
    "netFlag": ""
}

OUT = '/root/test1-gong/gong-lora-system/footprint/gong_server_schematic.json'
with open(OUT, 'w', encoding='utf-8') as f:
    json.dump(schematic, f, indent=2, ensure_ascii=False)

print(f'OK: {len(shapes)} shapes → {OUT}')
