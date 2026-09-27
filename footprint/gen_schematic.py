#!/usr/bin/env python3
"""Генератор EasyEDA Standard JSON схемы — универсальная плата узла Gong LoRa v1.

Источник истины — PCB_SPEC_PROMPT.md; сети совпадают с schematic_netlist.md.
Связи сделаны сетевыми метками (net labels): одинаковое имя = одна цепь.
"""
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

STUB = 30   # длина вывода

def passive(x, y, ref, value, net_top, net_bot, fill='#fff8e8', dnp=False):
    """Двухвыводной элемент вертикально: верхний вывод net_top, нижний net_bot."""
    rect(x, y, 30, 50, fill='#eeeeee' if dnp else fill)
    text(x+15, y+25, ref, size='7pt', bold='bold')
    text(x+35, y+20, value, anchor='left', size='7pt', color='#333333')
    if dnp:
        text(x+35, y+34, 'DNP', anchor='left', size='7pt', color='#cc0000', bold='bold')
    wire(x+15, y-15, x+15, y)
    netlabel(x+15, y-15, net_top, rot=270)
    wire(x+15, y+50, x+15, y+65)
    netlabel(x+15, y+65, net_bot, rot=90)

def block(x, y, w, ref, name, left=(), right=(), sub='', fill='#eef0ff', pitch=20):
    """Прямоугольный модуль. left/right — списки (имя вывода, сеть | None=NC)."""
    rows = max(len(left), len(right))
    top = 50 if sub else 40
    h = top + rows * pitch
    rect(x, y, w, h, fill=fill)
    text(x+w//2, y+14, ref, bold='bold', size='11pt')
    text(x+w//2, y+28, name, size='8pt')
    if sub:
        text(x+w//2, y+42, sub, size='7pt', color='#666666')
    for i, (pn, net) in enumerate(left):
        py = y + top + i * pitch
        wire(x-STUB, py, x, py)
        text(x+5, py, pn, anchor='left', size='7pt', color='#444444')
        if net:
            netlabel(x-STUB, py, net, rot=180)
        else:
            nc_mark(x-STUB, py)
    for i, (pn, net) in enumerate(right):
        py = y + top + i * pitch
        wire(x+w, py, x+w+STUB, py)
        text(x+w-5, py, pn, anchor='right', size='7pt', color='#444444')
        if net:
            netlabel(x+w+STUB, py, net, rot=0)
        else:
            nc_mark(x+w+STUB, py)

# ─────────────────────────────────────────
# TITLE
# ─────────────────────────────────────────
text(560, 22, 'GONG LoRa NODE v1.0 — universal board (server + client), see PCB_SPEC_PROMPT.md',
     anchor='center', size='13pt', bold='bold', color='#222222')

# ─────────────────────────────────────────
# U1 — ESP32-DevKitC-V4
# Порядок — официальный Espressif Getting Started Guide: пин 1 со стороны
# антенны, пин 19 со стороны USB. 5V — последний пин ЛЕВОГО ряда, GND —
# первый пин ПРАВОГО. 3V3 DevKit — NC: два LDO параллельно соединять нельзя.
# ─────────────────────────────────────────
u1_left = [
    ('3V3', None), ('EN', None), ('GPIO36', None), ('GPIO39', None),
    ('GPIO34', 'IO34'), ('GPIO35', 'IO35'),
    ('GPIO32', 'BUTTON'),
    ('GPIO33', 'I2S_DIN'), ('GPIO25', 'I2S_LRC'), ('GPIO26', 'I2S_BCLK'),
    ('GPIO27', 'RELAY_IN'),
    ('GPIO14', 'LORA_RST'),
    ('GPIO12', None),          # strapping
    ('GND', 'GND'),
    ('GPIO13', 'STATUS_LED_IO'),
    ('GPIO9', None), ('GPIO10', None), ('GPIO11', None),   # flash
    ('5V', '+5V'),
]
u1_right = [
    ('GND', 'GND'),
    ('GPIO23', 'LORA_MOSI'), ('GPIO22', 'I2C_SCL'),
    ('GPIO1', None), ('GPIO3', None),
    ('GPIO21', 'I2C_SDA'),
    ('GND', 'GND'),
    ('GPIO19', 'LORA_MISO'), ('GPIO18', 'LORA_SCK'), ('GPIO5', 'LORA_NSS'),
    ('GPIO17', 'IO17'), ('GPIO16', 'IO16'),
    ('GPIO4', 'LORA_DIO0'),
    ('GPIO0', None), ('GPIO2', None), ('GPIO15', None),   # strapping
    ('GPIO8', None), ('GPIO7', None), ('GPIO6', None),    # flash
]
block(330, 60, 160, 'U1', 'ESP32-DevKitC-V4', u1_left, u1_right)

# ─────────────────────────────────────────
# Питание: J1 → F1 → (RV1) → PS1 → D1 → +5V → U5 → +3V3
# ─────────────────────────────────────────
block(60, 60, 110, 'J1', 'AC IN 220V', right=[('L', 'AC_L_IN'), ('N', 'AC_N')],
      sub='5.08mm 250V', fill='#ffeeee')
passive(200, 70, 'F1', 'T1A 250V', 'AC_L_IN', 'AC_L', fill='#ffeeee')
passive(250, 70, 'RV1', '10D561K', 'AC_L', 'AC_N', fill='#ffeeee')
block(60, 180, 130, 'PS1', 'HLK-10M05', left=[('AC-L', 'AC_L'), ('AC-N', 'AC_N')],
      right=[('+Vo', '+5V_HLK'), ('-Vo', 'GND')], sub='AC-DC 5V / 2A', fill='#ffeeee')
passive(40, 300, 'D1', 'SS34 (A top)', '+5V_HLK', '+5V')
passive(110, 300, 'JP1', '0R 1206', '+5V_HLK', '+5V', dnp=True)
passive(180, 300, 'C1', '470uF/10V', '+5V', 'GND')
passive(250, 300, 'C2', '100nF', '+5V', 'GND')

block(60, 420, 120, 'U5', 'LD1117V33', left=[('VIN', '+5V')],
      right=[('VOUT/tab', '+3V3'), ('GND', 'GND')], sub='SOT-223')
passive(40, 540, 'C4', '10uF', '+5V', 'GND')
passive(100, 540, 'C5', '100nF', '+5V', 'GND')
passive(160, 540, 'C6', '10uF', '+3V3', 'GND')
passive(220, 540, 'C7', '100nF', '+3V3', 'GND')
passive(280, 540, 'C3', '22uF', '+3V3', 'GND')
passive(40, 650, 'R9', '2.2k', '+3V3', 'LED1_A')
passive(100, 650, 'LED1', 'green (A top)', 'LED1_A', 'GND')

# ─────────────────────────────────────────
# U2 — Ra-02 (2×4) + R6/R7/C9/C11
# ─────────────────────────────────────────
block(620, 60, 130, 'U2', 'Ra-02  SX1278',
      left=[('1 MISO', 'LORA_MISO'), ('3 SCK', 'LORA_SCK'), ('5 NSS', 'LORA_NSS'), ('7 RST', 'LORA_RST')],
      right=[('2 VCC', '+3V3'), ('4 MOSI', 'LORA_MOSI'), ('6 DIO0', 'LORA_DIO0'), ('8 GND', 'GND')])
passive(860, 60, 'R6', '10k', '+3V3', 'LORA_NSS')
passive(920, 60, 'R7', '10k', '+3V3', 'LORA_RST')
passive(980, 60, 'C9', '100nF', 'LORA_RST', 'GND')
passive(1040, 60, 'C11', '100nF', '+3V3', 'GND')

# ─────────────────────────────────────────
# U3 — MAX98357A (1×7) + FB1/C8/C10/R3
# ─────────────────────────────────────────
block(620, 200, 130, 'U3', 'MAX98357A', sub='I2S Amp (pin order: CHECK module)',
      left=[('LRC', 'I2S_LRC'), ('BCLK', 'I2S_BCLK'), ('DIN', 'I2S_DIN'), ('GAIN', None),
            ('SD', 'MAX_SD'), ('GND', 'GND'), ('VIN', '+5V_AUDIO')])
passive(860, 200, 'FB1', '600R@100MHz', '+5V', '+5V_AUDIO')
passive(920, 200, 'C8', '220uF/10V', '+5V_AUDIO', 'GND')
passive(980, 200, 'C10', '100nF', '+5V_AUDIO', 'GND')
passive(1040, 200, 'R3', '1M', '+5V_AUDIO', 'MAX_SD', dnp=True)

# ─────────────────────────────────────────
# U4 — DS3231 ZS-042 (1×4) + R1/R2 (DNP: подтяжки уже на модуле)
# ─────────────────────────────────────────
block(620, 420, 130, 'U4', 'DS3231 ZS-042', sub='RTC  I2C',
      left=[('GND', 'GND'), ('VCC', '+3V3'), ('SDA', 'I2C_SDA'), ('SCL', 'I2C_SCL')])
passive(860, 420, 'R1', '4.7k', '+3V3', 'I2C_SDA', dnp=True)
passive(920, 420, 'R2', '4.7k', '+3V3', 'I2C_SCL', dnp=True)

# ─────────────────────────────────────────
# Реле — J3 + R10/R11/R12 (GPIO27). Запаивается R11 ИЛИ R12.
# ─────────────────────────────────────────
block(620, 560, 130, 'J3', 'RELAY module', sub='JST-XH 3',
      left=[('1 +5V', '+5V'), ('2 GND', 'GND'), ('3 IN', 'RELAY_OUT')])
passive(860, 560, 'R10', '100R', 'RELAY_IN', 'RELAY_OUT')
passive(920, 560, 'R11', '10k act.HIGH', 'RELAY_IN', 'GND')
passive(1040, 560, 'R12', '10k act.LOW', '+3V3', 'RELAY_IN', dnp=True)

# ─────────────────────────────────────────
# Кнопка — SW1 + J4 + R13/R14/C12 (GPIO32)
# ─────────────────────────────────────────
block(330, 560, 130, 'J4', 'EXT BUTTON', sub='JST-XH 2',
      left=[('1 BTN', 'BTN'), ('2 GND', 'GND')])
passive(330, 700, 'SW1', 'tact 6x6', 'BTN', 'GND')
passive(390, 700, 'R13', '10k', '+3V3', 'BTN')
passive(450, 700, 'R14', '470R', 'BTN', 'BUTTON')
passive(510, 700, 'C12', '100nF', 'BUTTON', 'GND')

# ─────────────────────────────────────────
# LED статуса — R15 + LED2 + J5 (GPIO13)
# ─────────────────────────────────────────
passive(620, 700, 'R15', '1k', 'STATUS_LED_IO', 'STATUS_LED')
passive(680, 700, 'LED2', 'blue/yel (A top)', 'STATUS_LED', 'GND')
block(920, 690, 130, 'J5', 'EXT LED (opt.)', sub='JST-XH 2',
      left=[('1 LED+', 'STATUS_LED'), ('2 GND', 'GND')])

# ─────────────────────────────────────────
# J6 — резерв 1×6
# ─────────────────────────────────────────
block(1110, 690, 110, 'J6', 'SPARE 1x6', sub='2.54',
      left=[('1', '+3V3'), ('2', 'GND'), ('3 IO16', 'IO16'), ('4 IO17', 'IO17'),
            ('5 IO34', 'IO34'), ('6 IO35', 'IO35')])

# ─────────────────────────────────────────
# BUILD JSON
# ─────────────────────────────────────────
CANVAS = "CA~1000~1000~#ffffff~yes~#cccccc~10~1200~900~line~10~pixel~5~0~0"
schematic = {
    "head": {
        "type": "schematic",
        "title": "Gong LoRa Node v1",
        "description": "ESP32-DevKitC-V4 + Ra-02 + MAX98357A + DS3231 + HLK-10M05 + relay/button/LED (PCB_SPEC_PROMPT.md)",
        "canvas": CANVAS,
        "version": "6.5.38",
        "encryptedDataCompliant": False
    },
    "canvas": CANVAS,
    "shape": shapes,
    "BBox": None,
    "netFlag": ""
}

OUT = 'gong_server_schematic.json'
with open(OUT, 'w', encoding='utf-8') as f:
    json.dump(schematic, f, indent=2, ensure_ascii=False)

print(f'OK: {len(shapes)} shapes → {OUT}')
