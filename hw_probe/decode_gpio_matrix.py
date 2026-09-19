#!/usr/bin/env python3
"""Decode an ESP32-S3 GPIO-matrix dump into a pin map.

Recovers which peripheral signal a running firmware routed to each GPIO, without
touching the firmware. Read-only: halts the CPU for a few ms over the built-in
USB-JTAG, reads registers, resumes.

Usage (board plugged in, firmware running):
  OCD=~/Library/Arduino15/packages/esp32/tools/openocd-esp32/*/bin/openocd
  $OCD -f board/esp32s3-builtin.cfg -c init -c halt \
       -c "mdw 0x60004004 4" -c "mdw 0x60004020 4" -c "mdw 0x6000403C 2" \
       -c "mdw 0x60004554 49" -c "mdw 0x60004154 256" -c "mdw 0x60009004 49" \
       -c "mdw 0x60004074 49" -c resume -c shutdown > dump.txt 2>&1
  python3 decode_gpio_matrix.py dump.txt

Register map (ESP32-S3 TRM, GPIO base 0x60004000, IO_MUX base 0x60009000):
  0x4004 OUT, 0x4010 OUT1, 0x4020 ENABLE, 0x402C ENABLE1, 0x403C IN, 0x4040 IN1
  0x4074+4n  GPIO_PINn         (open-drain bit 2)
  0x4154+4n  FUNCn_IN_SEL_CFG  (n = input signal 0..255; bits0-5 gpio, bit7 via-matrix)
  0x4554+4n  FUNCn_OUT_SEL_CFG (n = gpio 0..48; bits0-8 output signal, 256 = plain GPIO)
  0x9004+4n  IO_MUX_GPIOn      (bits12-14 function, bit9 input-enable, bit8 pull-up, bit7 pull-down)
"""
import glob, re, sys

SIGMAP = glob.glob('/Users/*/Library/Arduino15/packages/esp32/tools/esp32s3-libs/*/include/soc/esp32s3/include/soc/gpio_sig_map.h')
sig = {}
if SIGMAP:
    for m in re.finditer(r'#define\s+(\w+?)_(IN|OUT)_IDX\s+(\d+)', open(SIGMAP[0]).read()):
        sig.setdefault((m.group(2), int(m.group(3))), []).append(m.group(1))
    # names that don't follow the _IN_IDX/_OUT_IDX pattern
    for m in re.finditer(r'#define\s+(LEDC_LS_SIG_OUT\d+)_IDX\s+(\d+)', open(SIGMAP[0]).read()):
        sig.setdefault(('OUT', int(m.group(2))), []).append(m.group(1))

regs = {}
for line in open(sys.argv[1]):
    m = re.match(r'0x([0-9a-f]{8}): (.*)', line)
    if m:
        a = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            regs[a + 4 * i] = int(w, 16)
r = regs.__getitem__
out = r(0x60004004) | (r(0x60004010) << 32)
en  = r(0x60004020) | (r(0x6000402C) << 32)
inp = r(0x6000403C) | (r(0x60004040) << 32)
print("outputs enabled :", [g for g in range(49) if en >> g & 1])
print("output level 1  :", [g for g in range(49) if out >> g & 1])
print("input level 1   :", [g for g in range(49) if inp >> g & 1])
print("\n== per-GPIO (only pins that are outputs or carry a peripheral signal) ==")
for g in range(49):
    v = r(0x60004554 + 4 * g); s = v & 0x1FF
    iom = r(0x60009004 + 4 * g); fn = (iom >> 12) & 7; ie = (iom >> 9) & 1; wpu = (iom >> 8) & 1; wpd = (iom >> 7) & 1
    od = (r(0x60004074 + 4 * g) >> 2) & 1
    if s == 256 and not en >> g & 1:
        continue
    desc = 'GPIO out (level %d)' % (out >> g & 1) if s == 256 else ','.join(sig.get(('OUT', s), ['sig%d' % s]))
    print(f"GPIO{g:2d}: {desc:34s} iomux_fn={fn} ie={ie} wpu={wpu} wpd={wpd} od={od}")
print("\n== input signals routed through the matrix ==")
for n in range(256):
    v = r(0x60004154 + 4 * n)
    if (v >> 7) & 1 and (v & 0x3F) < 49:
        print(f"  {','.join(sig.get(('IN', n), ['sig%d' % n])):22s} <- GPIO{v & 0x3F}")
