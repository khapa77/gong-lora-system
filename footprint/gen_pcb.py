#!/usr/bin/env python3
"""
Генератор EasyEDA Standard JSON — PCB layout (placement) для универсальной
платы узла Gong LoRa v1. Источник истины — PCB_SPEC_PROMPT.md; сети совпадают
с gen_schematic.py и schematic_netlist.md.

ВАЖНО (см. footprint/pcb_layout_notes.md для полного списка):
- Это ПЛЕЙСМЕНТ (расстановка footprint'ов по зонам + назначение сетей на пады),
  НЕ готовая трассировка. Медные дорожки этот скрипт не рисует — на каждый pad
  проставлена сеть (net), и EasyEDA покажет ratsnest. Трассировку делай в
  EasyEDA/KiCad с реальным DRC.
- Точные размеры ESP32-DevKitC-V4 (27.94 x 48.26 мм, ряды через 25.40 мм /
  1000 mil, шаг пинов 2.54 мм / 100 mil) — из esp32_devkitc_v4_dimensions.pdf.
- Остальные footprint'ы (HLK-10M05, SOT-223, SMA, электролиты, клеммники,
  держатель предохранителя, JST-XH, кнопка) — ОБОБЩЁННЫЕ. Перед заказом
  замени их библиотечными footprint'ами под купленные детали.
- Изоляция 220 В (≥ 6.4 мм + прорезь, ТЗ §6) здесь только обозначена
  маркерами на шелкографии — проверяется DRC в EDA, а не этим скриптом.
"""
import json

counter = [0]
def uid():
    counter[0] += 1
    return f'p{counter[0]:05d}'

shapes = []

# Layers (стандартная нумерация EasyEDA Standard PCB):
L_TOP, L_BOTTOM, L_TOP_SILK, L_BOTTOM_SILK = 1, 2, 3, 4
L_BOARD_OUTLINE, L_MULTI, L_DOCUMENT = 10, 11, 12

def pad(x, y, net, number, w=63, h=63, hole_r=17, shape='ELLIPSE', layer=L_MULTI, rot=0):
    # PAD~shape~x~y~w~h~layer~net~number~holeRadius~points~rotation~id~holeLength~holePoint~plated~locked
    shapes.append(f'PAD~{shape}~{x}~{y}~{w}~{h}~{layer}~{net}~{number}~{hole_r}~~{rot}~{uid()}~0~~Y~0')

def smd_pad(x, y, net, number, w, h, layer=L_TOP, rot=0):
    shapes.append(f'PAD~RECT~{x}~{y}~{w}~{h}~{layer}~{net}~{number}~0~~{rot}~{uid()}~0~~Y~0')

def track(points, net='', width=6, layer=L_TOP):
    pts = ' '.join(f'{x} {y}' for x, y in points)
    shapes.append(f'TRACK~{width}~{layer}~{net}~{pts}~{uid()}~0')

def text(x, y, s, layer=L_TOP_SILK, size='6pt', rot=0):
    s = str(s).replace('~', '-')
    shapes.append(f'T~N~{x}~{y}~4~{rot}~none~{layer}~~{size}~0~0~{s}~none~{uid()}~0')

def hole(x, y, dia):
    shapes.append(f'HOLE~{x}~{y}~{dia}~{uid()}~0')

def box(x, y, w, h, layer=L_TOP_SILK, width=4):
    pts = [(x, y), (x+w, y), (x+w, y+h), (x, y+h), (x, y)]
    track(pts, net='', width=width, layer=layer)

def zone_marker(x, y, w, h, label):
    # визуальная (не электрическая) граница зоны — silkscreen-прямоугольник
    box(x, y, w, h)
    text(x + 10, y - 20, label, size='7pt')

def header_1xN(x, y, nets, ref, name, pitch=100, label_dy=-25):
    """Однорядный THT-разъём/гнездо, вертикально сверху вниз, пады 1..N."""
    text(x - 10, y + label_dy, f'{ref} {name}', size='7pt')
    for i, net in enumerate(nets):
        pad(x, y + i * pitch, net, i + 1, shape='RECT' if i == 0 else 'ELLIPSE')

def header_row(x, y, nets, ref, name, pitch=100):
    """Однорядный разъём горизонтально слева направо, пады 1..N."""
    text(x - 10, y - 45, f'{ref} {name}', size='7pt')
    for i, net in enumerate(nets):
        pad(x + i * pitch, y, net, i + 1, shape='RECT' if i == 0 else 'ELLIPSE')

def smd2(x, y, ref, value, net1, net2, dnp=False, pitch=80, pw=50, ph=60):
    """Двухвыводной SMD (0805 по умолчанию), горизонтально: pad1 слева."""
    text(x - 30, y - 45, f'{ref} {value}' + (' DNP' if dnp else ''), size='5pt')
    smd_pad(x - pitch // 2, y, net1, 1, pw, ph)
    smd_pad(x + pitch // 2, y, net2, 2, pw, ph)

def electrolytic(x, y, ref, value, net_p, net_n, pitch=150):
    """THT радиальный электролит: pad1 (+) квадратный."""
    text(x - 30, y - 60, f'{ref} {value}', size='6pt')
    pad(x, y, net_p, 1, w=70, h=70, hole_r=18, shape='RECT')
    pad(x + pitch, y, net_n, 2, w=70, h=70, hole_r=18)
    text(x - 50, y + 10, '+', size='8pt')

def sot223(x, y, ref, name, gnd_net, vout_net, vin_net):
    """Обобщённый land pattern SOT-223: 3 лапки + таб (VOUT). Сверить с datasheet."""
    text(x - 150, y - 150, f'{ref} {name} (SOT-223)', size='6pt')
    lead_pitch = 90   # ~2.3 мм
    smd_pad(x - lead_pitch, y, gnd_net, 1, 35, 70)
    smd_pad(x,              y, vout_net, 2, 35, 70)
    smd_pad(x + lead_pitch, y, vin_net, 3, 35, 70)
    smd_pad(x, y - 120, vout_net, 4, 150, 90)   # таб

def tact_6x6(x, y, ref, net_a, net_b):
    """Тактовая кнопка 6×6 THT: пары 1–2 и 3–4 замкнуты внутри."""
    text(x - 20, y - 50, f'{ref} tact 6x6', size='6pt')
    for i, (dx, dy, net) in enumerate([(0, 0, net_a), (256, 0, net_a),
                                       (0, 177, net_b), (256, 177, net_b)]):
        pad(x + dx, y + dy, net, i + 1, w=70, h=70, hole_r=20)
    box(x - 20, y - 20, 296, 217, width=3)

def mounting_hole(x, y, dia=126):
    hole(x, y, dia)   # 3.2 мм под M3

# ═══════════════════════════════════════════════════════════════
# ДОСКА — ≤ 100 × 100 мм (ТЗ §1)
# ═══════════════════════════════════════════════════════════════
BOARD_W, BOARD_H = 3900, 3900   # mil (~99 x 99 мм)
box(0, 0, BOARD_W, BOARD_H, layer=L_BOARD_OUTLINE, width=6)
text(1300, 3860, 'GONG LoRa NODE v1.0', size='9pt')

# 4 × M3, центр в 157 mil (4 мм) от края; keep-out 6 мм без меди — задать в EDA
for mx, my in [(157, 157), (BOARD_W-157, 157), (157, BOARD_H-157), (BOARD_W-157, BOARD_H-157)]:
    mounting_hole(mx, my)

# ═══════════════════════════════════════════════════════════════
# ЗОНА A — 220 В (J1, F1, RV1, AC-сторона PS1). Левая полоса.
# ═══════════════════════════════════════════════════════════════
ZA_X, ZA_Y, ZA_W, ZA_H = 80, 450, 1000, 3000
zone_marker(ZA_X, ZA_Y, ZA_W, ZA_H, 'ZONE A 220V - creepage >= 6.4mm, no GND pour')
text(ZA_X + 20, ZA_Y + 60, '220 V', size='10pt')

# J1 — клеммник 5.08 мм (L, N), у края платы
text(ZA_X + 40, ZA_Y + 190, 'J1 AC IN  L / N', size='7pt')
pad(ZA_X + 150, ZA_Y + 300, 'AC_L_IN', 1, w=100, h=100, hole_r=26, shape='RECT')
pad(ZA_X + 150, ZA_Y + 500, 'AC_N',    2, w=100, h=100, hole_r=26)
text(ZA_X + 250, ZA_Y + 310, 'L', size='8pt')
text(ZA_X + 250, ZA_Y + 510, 'N', size='8pt')

# F1 — держатель предохранителя (обобщённо: 2 вывода, шаг ~22.6 мм для 5×20)
text(ZA_X + 480, ZA_Y + 700, 'F1 T1A 250V', size='7pt')
pad(ZA_X + 550, ZA_Y + 800,  'AC_L_IN', 1, w=100, h=100, hole_r=26)
pad(ZA_X + 550, ZA_Y + 1690, 'AC_L',    2, w=100, h=100, hole_r=26)

# RV1 — варистор 10D561K (опц.), шаг 7.5 мм, между AC_L и AC_N после F1
text(ZA_X + 100, ZA_Y + 1850, 'RV1 10D561K (opt.)', size='6pt')
pad(ZA_X + 200, ZA_Y + 1950, 'AC_L', 1, w=80, h=80, hole_r=20)
pad(ZA_X + 495, ZA_Y + 1950, 'AC_N', 2, w=80, h=80, hole_r=20)

# PS1 HLK-10M05 — обобщённый footprint: AC-выводы в зоне A, DC — за прорезью.
# Реальный шаг/размер взять из datasheet купленной ревизии!
PS_AC_X, PS_DC_X, PS_Y = ZA_X + 750, ZA_X + 1400, ZA_Y + 2350
text(PS_AC_X - 100, PS_Y - 150, 'PS1 HLK-10M05 (generic - CHECK datasheet)', size='7pt')
pad(PS_AC_X, PS_Y,       'AC_L', 1, w=100, h=100, hole_r=26)
pad(PS_AC_X, PS_Y + 400, 'AC_N', 2, w=100, h=100, hole_r=26)
pad(PS_DC_X, PS_Y,       '+5V_HLK', 3, w=100, h=100, hole_r=26)
pad(PS_DC_X, PS_Y + 400, 'GND',     4, w=100, h=100, hole_r=26)
box(PS_AC_X - 100, PS_Y - 100, PS_DC_X - PS_AC_X + 200, 600, width=3)

# Прорезь первичка/вторичка (фрезеровка 1.5–2 мм) — только обозначение
SLOT_X = ZA_X + ZA_W + 40
box(SLOT_X, ZA_Y, 70, ZA_H, layer=L_DOCUMENT, width=4)
text(SLOT_X - 40, ZA_Y - 60, 'SLOT 1.5-2mm (mill)', size='6pt')

# ═══════════════════════════════════════════════════════════════
# ЗОНА B — цифра + RF (U1, U2, U4, U5, реле/кнопка/LED/J6). Верх справа.
# ═══════════════════════════════════════════════════════════════
ZB_X, ZB_Y = SLOT_X + 150, 80
ZB_W, ZB_H = BOARD_W - ZB_X - 80, 2350
zone_marker(ZB_X, ZB_Y + 40, ZB_W, ZB_H, 'ZONE B digital + RF')

# U1 — ESP32-DevKitC-V4, 2 × гнездо 1×19. USB (пин 19) — к ВЕРХНЕМУ краю платы,
# модуль свисает за край, чтобы прошивать без извлечения.
# Порядок — официальный Espressif Getting Started Guide: пин 1 со стороны
# антенны, 5V — последний пин ЛЕВОГО ряда, GND — первый пин ПРАВОГО.
left_pins = ['NC',        # 1  3V3 — NC: два LDO параллельно нельзя
             'NC', 'NC', 'NC',             # EN, GPIO36, GPIO39
             'IO34', 'IO35',               # → J6
             'BUTTON',                     # GPIO32
             'I2S_DIN', 'I2S_LRC', 'I2S_BCLK',
             'RELAY_IN',                   # GPIO27
             'LORA_RST',                   # GPIO14
             'NC',                         # GPIO12 strapping
             'GND',
             'STATUS_LED_IO',              # GPIO13
             'NC', 'NC', 'NC',             # flash
             '+5V']                        # 19  5V
right_pins = ['GND', 'LORA_MOSI', 'I2C_SCL', 'NC', 'NC', 'I2C_SDA', 'GND',
              'LORA_MISO', 'LORA_SCK', 'LORA_NSS', 'IO17', 'IO16', 'LORA_DIO0',
              'NC', 'NC', 'NC',            # GPIO0, 2, 15 — strapping
              'NC', 'NC', 'NC']            # flash
ESP_LEFT_X, ESP_RIGHT_X = ZB_X + 250, ZB_X + 1250
ESP_PIN19_Y = ZB_Y + 250                    # пин 19 (USB) ближе всего к верхнему краю
def esp_y(i):   # i = 0..18 → пин 1..19; пин 1 внизу (антенна), пин 19 вверху (USB)
    return ESP_PIN19_Y + (18 - i) * 100
text(ESP_LEFT_X + 150, ESP_PIN19_Y - 150, 'U1 ESP32-DevKitC-V4  USB ^ board edge', size='7pt')
text(ESP_LEFT_X + 400, esp_y(0) - 50, 'ANT end (pin 1)', size='7pt')
for i, net in enumerate(left_pins):
    pad(ESP_LEFT_X, esp_y(i), net, i + 1, shape='RECT' if i == 0 else 'ELLIPSE')
for i, net in enumerate(right_pins):
    pad(ESP_RIGHT_X, esp_y(i), net, i + 20)
box(ESP_LEFT_X - 50, ESP_PIN19_Y - 100, 1100, 1900, width=3)   # контур модуля (приблизительно)

# U2 — Ra-02 на переходной 2×4, у правого края, антенный разъём к краю
U2_X, U2_Y = BOARD_W - 450, ZB_Y + 370
text(U2_X - 150, U2_Y - 90, 'U2 Ra-02 2x4   ANT ->', size='7pt')
ra_pins = [(1, 'LORA_MISO'), (2, '+3V3'), (3, 'LORA_SCK'), (4, 'LORA_MOSI'),
           (5, 'LORA_NSS'), (6, 'LORA_DIO0'), (7, 'LORA_RST'), (8, 'GND')]
for num, net in ra_pins:
    col, row = (num - 1) % 2, (num - 1) // 2
    pad(U2_X + col * 100, U2_Y + row * 100, net, num, shape='RECT' if num == 1 else 'ELLIPSE')
zone_marker(U2_X - 100, U2_Y - 150, 480, 600, 'no tracks under Ra-02')

# Обвязка Ra-02: R6, R7, C9, C3 + C11 у VCC
RR_X, RR_Y = U2_X - 50, U2_Y + 550
smd2(RR_X,       RR_Y,       'R6',  '10k',   '+3V3', 'LORA_NSS')
smd2(RR_X + 200, RR_Y,       'R7',  '10k',   '+3V3', 'LORA_RST')
smd2(RR_X,       RR_Y + 150, 'C9',  '100nF', 'LORA_RST', 'GND')
smd2(RR_X + 200, RR_Y + 150, 'C11', '100nF', '+3V3', 'GND')
smd2(RR_X + 100, RR_Y + 300, 'C3',  '22uF',  '+3V3', 'GND', pitch=110, pw=60, ph=70)

# U5 — LD1117V33 + C4..C7, между HLK и Ra-02, ближе к Ra-02
U5_X, U5_Y = ESP_RIGHT_X + 330, ZB_Y + 1570
sot223(U5_X, U5_Y, 'U5', 'LD1117V33', 'GND', '+3V3', '+5V')
smd2(U5_X - 100, U5_Y + 150, 'C6', '10uF',  '+3V3', 'GND')
smd2(U5_X + 100, U5_Y + 150, 'C7', '100nF', '+3V3', 'GND')
smd2(U5_X - 100, U5_Y + 300, 'C4', '10uF',  '+5V',  'GND')
smd2(U5_X + 100, U5_Y + 300, 'C5', '100nF', '+5V',  'GND')

# LED1 + R9 — индикатор +3V3
smd2(U5_X + 330, U5_Y - 70, 'R9',   '2.2k',  '+3V3', 'LED1_A')
smd2(U5_X + 330, U5_Y + 80, 'LED1', 'green', 'LED1_A', 'GND')

# U4 — DS3231 ZS-042, гнездо 1×4 (модуль ~38×22 мм нависает над платой)
U4_X, U4_Y = ESP_RIGHT_X + 230, ZB_Y + 270
header_1xN(U4_X, U4_Y, ['GND', '+3V3', 'I2C_SDA', 'I2C_SCL'], 'U4', 'DS3231 ZS-042')
text(U4_X + 80, U4_Y + 150, 'module ~38x22mm overhangs - check height', size='5pt')
smd2(U4_X + 80, U4_Y + 500, 'R1', '4.7k', '+3V3', 'I2C_SDA', dnp=True)
smd2(U4_X + 280, U4_Y + 500, 'R2', '4.7k', '+3V3', 'I2C_SCL', dnp=True)

# Разъёмы по краю зоны B (правый край, ниже Ra-02): J3 реле, J4 кнопка, J5 LED, J6 резерв
CON_X = BOARD_W - 250
header_1xN(CON_X, ZB_Y + 1420, ['+5V', 'GND', 'RELAY_OUT'], 'J3', 'RELAY +5V GND IN', pitch=98)
smd2(CON_X - 300, ZB_Y + 1420, 'R10', '100R', 'RELAY_IN', 'RELAY_OUT')
smd2(CON_X - 300, ZB_Y + 1570, 'R11', '10k act.HIGH', 'RELAY_IN', 'GND')
smd2(CON_X - 300, ZB_Y + 1720, 'R12', '10k act.LOW', 'RELAY_IN', '+3V3', dnp=True)
text(CON_X - 560, ZB_Y + 1820, 'R11 = relay active HIGH, R12 = active LOW - fit ONE', size='5pt')

# ═══════════════════════════════════════════════════════════════
# Нижний край зоны B: кнопка, LED статуса, J6
# ═══════════════════════════════════════════════════════════════
ROW_Y = ZB_Y + 2150
tact_6x6(ESP_LEFT_X - 100, ROW_Y, 'SW1', 'BTN', 'GND')
header_row(ESP_LEFT_X + 350, ROW_Y + 60, ['BTN', 'GND'], 'J4', 'BTN GND', pitch=98)
smd2(ESP_LEFT_X + 750, ROW_Y,       'R13', '10k',   '+3V3', 'BTN')
smd2(ESP_LEFT_X + 750, ROW_Y + 150, 'R14', '470R',  'BTN', 'BUTTON')
smd2(ESP_LEFT_X + 950, ROW_Y + 150, 'C12', '100nF', 'BUTTON', 'GND')

smd2(ESP_RIGHT_X + 300, ROW_Y,       'R15',  '1k',      'STATUS_LED_IO', 'STATUS_LED')
smd2(ESP_RIGHT_X + 300, ROW_Y + 150, 'LED2', 'status',  'STATUS_LED', 'GND')
header_row(ESP_RIGHT_X + 550, ROW_Y + 60, ['STATUS_LED', 'GND'], 'J5', 'LED + GND (opt.)', pitch=98)
header_1xN(CON_X, ZB_Y + 1900, ['+3V3', 'GND', 'IO16', 'IO17', 'IO34', 'IO35'],
           'J6', '3V3 GND 16 17 34 35')

# ═══════════════════════════════════════════════════════════════
# ЗОНА C — аудио (D1/C1 у выхода HLK, FB1, U3). Низ справа.
# ═══════════════════════════════════════════════════════════════
ZC_X, ZC_Y = ZB_X, ZB_Y + ZB_H + 150
ZC_W, ZC_H = ZB_W, BOARD_H - ZC_Y - 250
zone_marker(ZC_X, ZC_Y, ZC_W, ZC_H, 'ZONE C audio - GND star at C1')

# D1 SS34 (SMA) + JP1 (DNP) + C1/C2 — сразу за DC-выходом HLK
D1_X, D1_Y = ZC_X + 500, ZC_Y + 250
smd2(D1_X, D1_Y, 'D1', 'SS34 (K right)', '+5V_HLK', '+5V', pitch=200, pw=100, ph=90)
smd2(D1_X, D1_Y + 200, 'JP1', '0R 1206', '+5V_HLK', '+5V', dnp=True, pitch=140, pw=60, ph=70)
electrolytic(D1_X + 300, D1_Y, 'C1', '470uF/10V', '+5V', 'GND', pitch=138)
smd2(D1_X + 400, D1_Y + 200, 'C2', '100nF', '+5V', 'GND')
text(D1_X + 250, D1_Y + 330, 'GND star point', size='6pt')

# FB1 + U3 MAX98357A (гнездо 1×7) + C8/C10/R3
FB1_X = ZC_X + 1300
smd2(FB1_X, ZC_Y + 250, 'FB1', '600R 2A', '+5V', '+5V_AUDIO')
U3_X, U3_Y = ZC_X + 1700, ZC_Y + 200
header_1xN(U3_X, U3_Y, ['I2S_LRC', 'I2S_BCLK', 'I2S_DIN', 'NC', 'MAX_SD', 'GND', '+5V_AUDIO'],
           'U3', 'MAX98357A LRC BCLK DIN GAIN SD GND VIN (CHECK)')
electrolytic(U3_X + 250, U3_Y + 600, 'C8', '220uF/10V', '+5V_AUDIO', 'GND', pitch=100)
smd2(U3_X + 300, U3_Y + 450, 'C10', '100nF', '+5V_AUDIO', 'GND')
smd2(U3_X + 300, U3_Y + 300, 'R3',  '1M', '+5V_AUDIO', 'MAX_SD', dnp=True)
text(U3_X + 500, U3_Y + 100, 'speaker -> module terminal, board edge', size='6pt')

# ═══════════════════════════════════════════════════════════════
# BUILD JSON
# ═══════════════════════════════════════════════════════════════
CANVAS = "CA~1000~1000~#ffffff~yes~#cccccc~10~1200~900~line~10~mil~5~0~0"
pcb = {
    "head": {
        "type": "pcb",
        "title": "Gong LoRa Node v1 — PCB",
        "description": "Placement/zoning draft — pads + net assignment, no routed copper (see gen_pcb.py docstring)",
        "canvas": CANVAS,
        "version": "6.5.38",
        "layers": [],
        "DRCRULE": None,
        "encryptedDataCompliant": False
    },
    "canvas": CANVAS,
    "shape": shapes,
    "BBox": None,
    "netFlag": ""
}

OUT = 'gong_server_pcb.json'
with open(OUT, 'w', encoding='utf-8') as f:
    json.dump(pcb, f, indent=2, ensure_ascii=False)

print(f'OK: {len(shapes)} shapes -> {OUT}')
