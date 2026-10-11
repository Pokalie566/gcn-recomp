"""Executes what ppc.translate() writes for instructions that are easy to get wrong -- the
carry, rotate masks that wrap, shifts of 32 and more, divisions at the limits, float rounding
and saturation, paired singles -- and checks the CPU state against the PowerPC manual.

    python3 recomp/test_ppc.py        (needs a C compiler: $CC, else cc)

Each case: an instruction word, the registers before, the registers expected after.
"""
import os
import struct
import subprocess
import sys
import tempfile

import ppc

HERE = os.path.dirname(os.path.abspath(__file__))


def D(op, d, a, imm):
    return op << 26 | d << 21 | a << 16 | (imm & 0xFFFF)


def X(xo, d, a, b, rc=0, op=31):
    return op << 26 | d << 21 | a << 16 | b << 11 | xo << 1 | rc


def XO(xo, d, a, b, rc=0):
    return 31 << 26 | d << 21 | a << 16 | b << 11 | xo << 1 | rc


def M(op, s, a, sh, mb, me, rc=0):
    return op << 26 | s << 21 | a << 16 | sh << 11 | mb << 6 | me << 1 | rc


def A(op, xo, d, a, b, c=0):
    return op << 26 | d << 21 | a << 16 | b << 11 | c << 6 | xo << 1


def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]


CA = 'xer_ca'
CASES = [
    # name, word, before, after
    ('addc carry out', XO(10, 3, 4, 5), {'r4': 0xFFFFFFFF, 'r5': 1}, {'r3': 0, CA: 1}),
    ('adde with carry in', XO(138, 3, 4, 5), {'r4': 1, 'r5': 1, CA: 1}, {'r3': 3, CA: 0}),
    ('subfc borrow (CA=0)', XO(8, 3, 4, 5), {'r4': 1, 'r5': 0}, {'r3': 0xFFFFFFFF, CA: 0}),
    ('subfc no borrow (CA=1)', XO(8, 3, 4, 5), {'r4': 0, 'r5': 5}, {'r3': 5, CA: 1}),
    ('subfe CA=0', XO(136, 3, 4, 5), {'r4': 5, 'r5': 10, CA: 0}, {'r3': 4, CA: 1}),
    ('subfic', D(8, 3, 4, 1), {'r4': 2}, {'r3': 0xFFFFFFFF, CA: 0}),
    ('srawi -1 >> 1: CA', X(824, 4, 3, 1), {'r4': 0xFFFFFFFF}, {'r3': 0xFFFFFFFF, CA: 1}),
    ('srawi -2 >> 1: no CA', X(824, 4, 3, 1), {'r4': 0xFFFFFFFE}, {'r3': 0xFFFFFFFF, CA: 0}),
    ('srawi positive', X(824, 4, 3, 1), {'r4': 5, CA: 1}, {'r3': 2, CA: 0}),
    ('sraw by 40, negative', X(792, 4, 3, 5), {'r4': 0x80000000, 'r5': 40}, {'r3': 0xFFFFFFFF, CA: 1}),
    ('sraw by 40, positive', X(792, 4, 3, 5), {'r4': 0x7FFFFFFF, 'r5': 40}, {'r3': 0, CA: 0}),
    ('slw by 32', X(24, 4, 3, 5), {'r4': 0xFFFFFFFF, 'r5': 32}, {'r3': 0}),
    ('srw by 33', X(536, 4, 3, 5), {'r4': 0xFFFFFFFF, 'r5': 33}, {'r3': 0}),
    ('srw by 31', X(536, 4, 3, 5), {'r4': 0x80000000, 'r5': 31}, {'r3': 1}),
    ('rlwinm wrapping mask', M(21, 4, 3, 0, 31, 0), {'r4': 0xFFFFFFFF}, {'r3': 0x80000001}),
    ('rotlwi 8', M(21, 4, 3, 8, 0, 31), {'r4': 0x12345678}, {'r3': 0x34567812}),
    ('rlwimi', M(20, 4, 3, 8, 16, 23), {'r3': 0xAAAAAAAA, 'r4': 0x12}, {'r3': 0xAAAA12AA}),
    ('rlwnm by 36 (= 4)', M(23, 4, 3, 5, 0, 31), {'r4': 0x12345678, 'r5': 36}, {'r3': 0x23456781}),
    ('cntlzw 0', X(26, 4, 3, 0), {'r4': 0}, {'r3': 32}),
    ('cntlzw 1', X(26, 4, 3, 0), {'r4': 1}, {'r3': 31}),
    ('extsb', X(954, 4, 3, 0), {'r4': 0x180}, {'r3': 0xFFFFFF80}),
    ('extsh', X(922, 4, 3, 0), {'r4': 0x18000}, {'r3': 0xFFFF8000}),
    ('divw truncates', XO(491, 3, 4, 5), {'r4': (-7) & 0xFFFFFFFF, 'r5': 2}, {'r3': (-3) & 0xFFFFFFFF}),
    ('divwu', XO(459, 3, 4, 5), {'r4': 0xFFFFFFFF, 'r5': 2}, {'r3': 0x7FFFFFFF}),
    ('divw by 0 does not trap', XO(491, 3, 4, 5), {'r4': 7, 'r5': 0}, {}),
    ('divw INT_MIN / -1 does not trap', XO(491, 3, 4, 5), {'r4': 0x80000000, 'r5': 0xFFFFFFFF}, {}),
    ('divwu by 0 does not trap', XO(459, 3, 4, 5), {'r4': 7, 'r5': 0}, {}),
    ('mulhw', XO(75, 3, 4, 5), {'r4': 0x80000000, 'r5': 2}, {'r3': 0xFFFFFFFF}),
    ('mulhwu', XO(11, 3, 4, 5), {'r4': 0xFFFFFFFF, 'r5': 0xFFFFFFFF}, {'r3': 0xFFFFFFFE}),
    ('mullw wraps', XO(235, 3, 4, 5), {'r4': 0x10000, 'r5': 0x10001}, {'r3': 0x10000}),
    ('cmpw signed', X(0, 0, 4, 5), {'r4': 0xFFFFFFFF, 'r5': 1}, {'cr0': 8}),
    ('cmplw unsigned', X(32, 0, 4, 5), {'r4': 0xFFFFFFFF, 'r5': 1}, {'cr0': 4}),
    ('cmpwi into cr7', D(11, 7 << 2, 4, -1), {'r4': 0xFFFFFFFF}, {'cr7': 2}),
    ('add. sets cr0 from the result', XO(266, 3, 4, 5, rc=1), {'r4': 0xFFFFFFFF, 'r5': 0}, {'r3': 0xFFFFFFFF, 'cr0': 8}),
    # Floating point (default FPSCR: round to nearest).
    ('frsp rounds to single', X(12, 1, 0, 2, op=63), {'f2': 1.0 / 3}, {'f1': f32(1.0 / 3)}),
    ('fctiwz 2.9', X(15, 1, 0, 2, op=63), {'f2': 2.9}, {'f1u': 2}),
    ('fctiwz -2.9', X(15, 1, 0, 2, op=63), {'f2': -2.9}, {'f1u': 0xFFFFFFFE}),
    ('fctiwz saturates', X(15, 1, 0, 2, op=63), {'f2': 1e10}, {'f1u': 0x7FFFFFFF}),
    ('fctiwz saturates low', X(15, 1, 0, 2, op=63), {'f2': -1e10}, {'f1u': 0x80000000}),
    ('fctiwz NaN', X(15, 1, 0, 2, op=63), {'f2': float('nan')}, {'f1u': 0x80000000}),
    ('fctiw 2.5 -> 2 (ties to even)', X(14, 1, 0, 2, op=63), {'f2': 2.5}, {'f1u': 2}),
    ('fctiw 3.5 -> 4', X(14, 1, 0, 2, op=63), {'f2': 3.5}, {'f1u': 4}),
    ('fmadd a*c+b', A(63, 29, 1, 2, 3, 4), {'f2': 3.0, 'f3': 1.0, 'f4': 2.0}, {'f1': 7.0}),
    ('fnmsub -(a*c-b)', A(63, 30, 1, 2, 3, 4), {'f2': 3.0, 'f3': 1.0, 'f4': 2.0}, {'f1': -5.0}),
    ('fmadds rounds to single', A(59, 29, 1, 2, 3, 4), {'f2': 1.0 / 3, 'f3': 0.0, 'f4': 1.0}, {'f1': f32(1.0 / 3)}),
    ('fdivs', A(59, 18, 1, 2, 3), {'f2': 1.0, 'f3': 3.0}, {'f1': f32(1.0 / 3)}),
    # Paired singles: ps0 is f[n], ps1 is ps1[n].
    ('ps_merge01', X(560, 1, 2, 3, op=4), {'f2': 1.0, 'p2': 2.0, 'f3': 3.0, 'p3': 4.0}, {'f1': 1.0, 'p1': 4.0}),
    ('ps_merge10', X(592, 1, 2, 3, op=4), {'f2': 1.0, 'p2': 2.0, 'f3': 3.0, 'p3': 4.0}, {'f1': 2.0, 'p1': 3.0}),
    ('ps_merge10 same register', X(592, 2, 2, 2, op=4), {'f2': 1.0, 'p2': 2.0}, {'f2': 2.0, 'p2': 1.0}),
    ('ps_sum0', A(4, 10, 1, 2, 3, 4), {'f2': 1.0, 'p3': 10.0, 'p4': 7.0}, {'f1': 11.0, 'p1': 7.0}),
    ('ps_sum1', A(4, 11, 1, 2, 3, 4), {'f2': 1.0, 'p3': 10.0, 'f4': 5.0}, {'f1': 5.0, 'p1': 11.0}),
    ('ps_madd', A(4, 29, 1, 2, 3, 4), {'f2': 2.0, 'p2': 3.0, 'f3': 1.0, 'p3': 1.0, 'f4': 4.0, 'p4': 5.0},
     {'f1': 9.0, 'p1': 16.0}),
    ('ps_muls0', A(4, 12, 1, 2, 0, 4), {'f2': 2.0, 'p2': 3.0, 'f4': 5.0, 'p4': 100.0}, {'f1': 10.0, 'p1': 15.0}),
    ('ps_add rounds to single', A(4, 21, 1, 2, 3), {'f2': 1.0 / 3, 'p2': 0.0, 'f3': 0.0, 'p3': 0.0},
     {'f1': f32(1.0 / 3), 'p1': 0.0}),
]


def lvalue(name):
    if name == CA:
        return 'c->xer_ca'
    kind, n = name[0], name[1:]
    if name.startswith('cr'):
        return f'c->cr[{name[2:]}]'
    if name.endswith('u'):   # an FPR's low word, where fctiw* puts its integer
        return f'(uint32_t)c->f[{name[1:-1]}].u'
    return {'r': f'c->r[{n}]', 'f': f'c->f[{n}].d', 'p': f'c->ps1[{n}]'}[kind]


def literal(name, v):
    if name[0] in 'fp' and not name.endswith('u'):
        return 'NAN' if v != v else repr(float(v))
    return f'0x{v:X}u'


def main():
    src = ['#include "recomp.h"', '#include <stdio.h>',
           'uint8_t* g_mem;',
           'uint32_t mmio_read32(uint32_t a) { return 0; } uint16_t mmio_read16(uint32_t a) { return 0; }',
           'uint8_t mmio_read8(uint32_t a) { return 0; } void mmio_write32(uint32_t a, uint32_t v) {}',
           'void mmio_write16(uint32_t a, uint16_t v) {} void mmio_write8(uint32_t a, uint8_t v) {}',
           'static int fails;',
           'static void eq(const char* t, const char* what, double got, double want) {',
           '    if (got != want && !(got != got && want != want)) { printf("FAIL %s: %s = %.17g, expected %.17g\\n", t, what, got, want); fails++; }',
           '}']
    for k, (name, word, before, after) in enumerate(CASES):
        snippet = ppc.translate(0x80003100, word)
        src.append(f'static void case{k}(CPU* c) {{')
        for r, v in before.items():
            src.append(f'    {lvalue(r)} = {literal(r, v)};')
        src.append(f'    {snippet}')
        for r, v in after.items():
            src.append(f'    eq("{name}", "{r}", (double){lvalue(r)}, (double){literal(r, v)});')
        src.append('}')
    src.append('int main(void) {')
    src.append('    static CPU cpu;')
    for k in range(len(CASES)):
        src.append(f'    memset(&cpu, 0, sizeof cpu); case{k}(&cpu);')
    src.append('    if (!fails) printf("ok: %d instructions\\n", ' + str(len(CASES)) + ');')
    src.append('    return fails != 0;')
    src.append('}')
    with tempfile.TemporaryDirectory() as tmp:
        c_path, exe = os.path.join(tmp, 't.c'), os.path.join(tmp, 't')
        with open(c_path, 'w') as f:
            f.write('\n'.join(src) + '\n')
        cc = os.environ.get('CC', 'cc')
        subprocess.run([cc, '-std=c11', '-O1', '-I', os.path.join(HERE, '..', 'runtime'), c_path, '-o', exe, '-lm'],
                       check=True)
        return subprocess.run([exe]).returncode


if __name__ == '__main__':
    sys.exit(main())
