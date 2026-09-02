#!/usr/bin/env python3
"""
Генератор EasyEDA Standard JSON — PCB layout (placement) для Gong LoRa Server.

ВАЖНО (см. footprint/pcb_layout_notes.md для полного списка):
- Это ПЛЕЙСМЕНТ (расстановка footprint'ов по зонам + назначение сетей на пады),
  НЕ готовая трассировка. Медные дорожки этот скрипт не рисует — вместо этого
  на каждый pad проставлена сеть (net), и EasyEDA сама покажет ratsnest
  (воздушные линии) между падами одной сети. Трассировку делай руками в
  EasyEDA (или автороутером) — так безопаснее, чем доверять сгенерированной
  геометрии меди без DRC-проверки.
- Точные размеры ESP32-DevKitC-V4 (27.94 x 48.26 мм, шаг рядов 25.40мм/1000mil,
  шаг пинов 2.54мм/100mil) взяты из footprint/esp32_devkitc_v4_dimensions.pdf —
  этим цифрам можно доверять.
- Остальные footprint'ы (HLK-10M05, SOT-223 AMS1117-3.3, электролиты, клеммники,
  ferrite bead) — ОБОБЩЁННЫЕ/приблизительные посадочные места. Перед заказом
  платы замени их на реальные библиотечные footprint'ы EasyEDA/JLCPCB для
  конкретных выбранных деталей.
- Зазор между зоной A (220В) и зоной B — заложен как ориентир (200mil/~5мм),
  это НЕ сертифицированное значение creepage/clearance. Для платы с сетевым
  напряжением проверь реальное требование (IEC 62368-1 и т.п.) перед фабрикацией.
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

def hole(x, y, dia, note=''):
    shapes.append(f'HOLE~{x}~{y}~{dia}~{uid()}~0')

def board_outline(x, y, w, h):
    pts = [(x, y), (x+w, y), (x+w, y+h), (x, y+h), (x, y)]
    track(pts, net='', width=6, layer=L_BOARD_OUTLINE)

def keepout_marker(x, y, w, h, label):
    # визуальная (не электрическая) граница зоны — просто silkscreen-прямоугольник
    pts = [(x, y), (x+w, y), (x+w, y+h), (x, y+h), (x, y)]
    track(pts, net='', width=4, layer=L_TOP_SILK)
    text(x + 10, y - 20, label, size='7pt')

def header(x, y, rows, cols, pitch, nets, ref, name, col_pitch=None):
    """
    THT-разъём rows x cols, шаг pitch (мил) между пинами в столбце,
    col_pitch — расстояние между столбцами (по умолчанию = pitch).
    nets — список имён сетей по пинам, порядок: сверху вниз левый столбец,
    затем сверху вниз правый столбец (для 2-колоночных), либо просто по порядку
    для 1-колоночных.
    """
    col_pitch = col_pitch or pitch
    text(x - 10, y - 25, f'{ref} {name}', size='7pt')
    idx = 0
    for c in range(cols):
        for r in range(rows):
            px = x + c * col_pitch
            py = y + r * pitch
            net = nets[idx] if idx < len(nets) else ''
            num = idx + 1
            pad(px, py, net, num)
            idx += 1

def sot223(x, y, ref, name, gnd_net, vout_net, vin_net):
    """
    Упрощённый land pattern SOT-223 (3 gull-wing лапки + таб).
    Реальные размеры зависят от выбранного производителя корпуса —
    сверься с datasheet AMS1117-3.3 перед фабрикацией.
    """
    text(x - 10, y - 30, f'{ref} {name} (SOT-223, ориентировочный footprint)', size='7pt')
    lead_pitch = 90   # ~2.3mm — типично для SOT-223
    lead_w, lead_h = 35, 70
    tab_w, tab_h = 150, 90
    # 3 лапки в ряд (GND, NC/VOUT-lead, VIN) — фактически GND слева, VIN справа
    smd_pad(x - lead_pitch, y, gnd_net, 1, lead_w, lead_h)
    smd_pad(x,               y, vout_net, 2, lead_w, lead_h)   # средняя лапка тоже VOUT
    smd_pad(x + lead_pitch, y, vin_net, 3, lead_w, lead_h)
    # таб (VOUT + теплоотвод) сзади, по центру
    smd_pad(x, y + 110, vout_net, 4, tab_w, tab_h)

def passive_th(x, y, ref, value, net_top, net_bot, pitch=100):
    text(x - 15, y - 20, f'{ref} {value}', size='6pt')
    pad(x, y, net_top, 1, w=55, h=55, hole_r=13)
    pad(x, y + pitch, net_bot, 2, w=55, h=55, hole_r=13)

def terminal_2(x, y, ref, name, net_a, net_b, pitch=200):
    text(x - 15, y - 25, f'{ref} {name}', size='7pt')
    pad(x, y, net_a, 1, w=90, h=90, hole_r=35)
    pad(x, y + pitch, net_b, 2, w=90, h=90, hole_r=35)

def mounting_hole(x, y, dia=126):
    hole(x, y, dia)

# ═══════════════════════════════════════════════════════════════
# ДОСКА
# ═══════════════════════════════════════════════════════════════
BOARD_W, BOARD_H = 4600, 3600   # mil (~116.8 x 91.4 мм)
board_outline(0, 0, BOARD_W, BOARD_H)
text(BOARD_W//2 - 400, -60, 'Gong LoRa Server — PCB placement v0.1 (draft, unrouted)', size='9pt')

for mx, my in [(120, 120), (BOARD_W-120, 120), (120, BOARD_H-120), (BOARD_W-120, BOARD_H-120)]:
    mounting_hole(mx, my, dia=126)  # M3-ish, проверь под реальный крепёж/корпус

# ═══════════════════════════════════════════════════════════════
# ЗОНА A — 220В (HLK-10M05 + предохранитель + клеммники L/N)
# ═══════════════════════════════════════════════════════════════
ZA_X, ZA_Y, ZA_W, ZA_H = 100, 200, 1100, 3200
keepout_marker(ZA_X, ZA_Y, ZA_W, ZA_H, 'ZONE A: 220V mains (HLK-10M05) — creepage/clearance TBD, verify vs IEC 62368-1')

terminal_2(ZA_X + 150, ZA_Y + 150, 'F1', 'Fuse 1A holder', 'AC_L_IN', 'AC_L', pitch=250)
terminal_2(ZA_X + 550, ZA_Y + 150, 'J1', 'AC IN (L/N)', 'AC_L_IN', 'AC_N', pitch=250)

# HLK-10M05 — обобщённый 4-выводной footprint (реальный шаг уточни по datasheet)
text(ZA_X + 100, ZA_Y + 700, 'PS1 HLK-10M05 (обобщённый footprint — сверь с datasheet!)', size='7pt')
pad(ZA_X + 150, ZA_Y + 800, 'AC_L', 1, w=90, h=90, hole_r=35)
pad(ZA_X + 150, ZA_Y + 1000, 'AC_N', 2, w=90, h=90, hole_r=35)
pad(ZA_X + 700, ZA_Y + 800, '+5V', 3, w=90, h=90, hole_r=35)
pad(ZA_X + 700, ZA_Y + 1000, 'GND', 4, w=90, h=90, hole_r=35)

# Bulk-конденсаторы на выходе HLK (C1, C2) — близко к +Vo/-Vo
passive_th(ZA_X + 350, ZA_Y + 1300, 'C1', '100uF/10V', '+5V', 'GND')
passive_th(ZA_X + 550, ZA_Y + 1300, 'C2', '100nF', '+5V', 'GND')

text(ZA_X + 50, ZA_Y + ZA_H - 60,
     'GND star-point здесь -> одна точка соединения с Zone B/C GND', size='6pt')

# ═══════════════════════════════════════════════════════════════
# ЗОНА B — цифра/RF (ESP32, AMS1117-3.3, Ra-02, DS3231)
# ═══════════════════════════════════════════════════════════════
ZB_X = ZA_X + ZA_W + 200   # +200mil ~5мм зазор изоляции от Zone A (проверь требования!)
ZB_W = 2400
keepout_marker(ZB_X, ZA_Y, ZB_W, ZA_H, 'ZONE B: digital/RF (ESP32 + LoRa + RTC + 3V3 LDO)')

# U1 — ESP32 DevKitC-V4 (реальные размеры из datasheet: 27.94x48.26мм = 1100x1900mil,
# шаг рядов 1000mil, шаг пинов 100mil)
ESP_LEFT_X  = ZB_X + 300
ESP_RIGHT_X = ESP_LEFT_X + 1000
ESP_Y = ZA_Y + 150

# lora-ds-autonomy: порядок сверен с официальным Espressif ESP32-DevKitC-V4
# Getting Started Guide — прежняя версия была сдвинута на 1 позицию и путала
# местами GND/5V между колонками (см. connect.md для разбора). 5V физически —
# последний пин ЛЕВОЙ колонки (не первый пин правой), GND — первый пин ПРАВОЙ
# (не левой). Порядок в списке = физический порядок пинов сверху вниз в
# колонке, начиная от края с USB-разъёмом.
left_pins = ['+3V3','EN_NC','NC','NC','NC','NC','NC',
             'I2S_DIN','I2S_LRC','I2S_BCLK','NC','LORA_RST','NC','GND','NC','NC','NC','NC','+5V']
right_pins = ['GND','LORA_MOSI','I2C_SCL','NC','NC','I2C_SDA','GND',
              'LORA_MISO','LORA_SCK','LORA_NSS','NC','NC','LORA_DIO0','NC','NC','NC','NC','NC','NC']

text(ESP_LEFT_X - 40, ESP_Y - 40, 'U1 ESP32-DevKitC-V4 (socket, 2x19 female header)', size='7pt')
for i, net in enumerate(left_pins):
    pad(ESP_LEFT_X, ESP_Y + i*100, net, i+1)
for i, net in enumerate(right_pins):
    pad(ESP_RIGHT_X, ESP_Y + i*100, net, i+20)

# U5 — AMS1117-3.3 (SOT-223) + входные/выходные конденсаторы, у правого края ESP32
U5_X, U5_Y = ESP_RIGHT_X + 300, ESP_Y + 200
sot223(U5_X, U5_Y, 'U5', 'AMS1117-3.3', 'GND', '+3V3', '+5V')
passive_th(U5_X - 200, U5_Y - 150, 'C4', '10uF/10V', '+5V', 'GND')
passive_th(U5_X - 80,  U5_Y - 150, 'C5', '100nF',    '+5V', 'GND')
passive_th(U5_X + 220, U5_Y - 150, 'C6', '10uF/6.3V','+3V3','GND')
passive_th(U5_X + 340, U5_Y - 150, 'C7', '100nF',    '+3V3','GND')

# I2C pullups рядом с DS3231
R_X, R_Y = U5_X - 100, U5_Y + 400
passive_th(R_X,        R_Y, 'R1', '4.7k', '+3V3', 'I2C_SDA')
passive_th(R_X + 150,  R_Y, 'R2', '4.7k', '+3V3', 'I2C_SCL')

# U4 — DS3231 (1x4), рядом с I2C-пинами ESP32
U4_X, U4_Y = R_X, R_Y + 300
header(U4_X, U4_Y, rows=4, cols=1, pitch=100,
       nets=['GND', '+3V3', 'I2C_SDA', 'I2C_SCL'], ref='U4', name='DS3231 ZS-042')

# U2 — Ra-02 (2x4), у правого края zone B / края платы — под SMA-антенну
U2_X, U2_Y = ZB_X + ZB_W - 500, ESP_Y + 300
header(U2_X, U2_Y, rows=4, cols=2, pitch=100, col_pitch=100, ref='U2', name='Ra-02 SX1278 (SMA — оставь зазор до края платы!)',
       nets=['LORA_MISO', 'LORA_SCK', 'LORA_NSS', 'LORA_RST',
             '+3V3', 'LORA_MOSI', 'LORA_DIO0', 'GND'])
keepout_marker(U2_X - 50, U2_Y - 350, 700, 700, 'antenna keep-out (SMA + кабель)')

# R6/R7 — NSS/RST pull-ups; C9 — RST snubber (рядом с U2, ниже antenna keep-out)
RR_X, RR_Y = U2_X, U2_Y + 400
passive_th(RR_X,       RR_Y,       'R6', '10k',   '+3V3',     'LORA_NSS')
passive_th(RR_X + 150, RR_Y,       'R7', '10k',   '+3V3',     'LORA_RST')
passive_th(RR_X + 150, RR_Y + 250, 'C9', '100nF', 'LORA_RST', 'GND')

# ═══════════════════════════════════════════════════════════════
# ЗОНА C — аудио (MAX98357A), физически подальше от Ra-02 (внизу)
# ═══════════════════════════════════════════════════════════════
ZC_X = ZB_X
ZC_Y = ZA_Y + ZA_H + 100
ZC_W, ZC_H = ZB_W, BOARD_H - ZC_Y - 150
keepout_marker(ZC_X, ZC_Y, ZC_W, ZC_H, 'ZONE C: audio (MAX98357A) — max расстояние от Zone B/RF')

FB1_X, FB1_Y = ZC_X + 200, ZC_Y + 150
passive_th(FB1_X, FB1_Y, 'FB1', 'ferrite 600R', '+5V', '+5V_AUDIO', pitch=100)

U3_X, U3_Y = FB1_X + 400, ZC_Y + 150
header(U3_X, U3_Y, rows=7, cols=1, pitch=100, ref='U3', name='MAX98357A',
       nets=['I2S_LRC', 'I2S_BCLK', 'I2S_DIN', 'NC', 'MAX_SD', 'GND', '+5V_AUDIO'])

passive_th(U3_X + 300, U3_Y, 'C8', '220uF/10V', '+5V_AUDIO', 'GND', pitch=150)
passive_th(U3_X + 450, U3_Y, 'R3', '1M', '+5V_AUDIO', 'MAX_SD')

terminal_2(U3_X + 650, U3_Y, 'SPK1', 'Speaker 4R', 'SPK_P', 'SPK_N', pitch=200)

# GND star-point note
text(ZC_X + 50, ZC_Y + ZC_H - 40,
     'Zone C GND соединяй ОДНИМ проводником со star-point (не полигоном насквозь под Zone B)', size='6pt')

# ═══════════════════════════════════════════════════════════════
# BUILD JSON
# ═══════════════════════════════════════════════════════════════
CANVAS = "CA~1000~1000~#ffffff~yes~#cccccc~10~1200~900~line~10~mil~5~0~0"
pcb = {
    "head": {
        "type": "pcb",
        "title": "Gong LoRa Server — PCB",
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
