#!/usr/bin/env python3
"""Locate the effect programmer's DLL reads in a build other than the reference one.

`EffectProgrammer` does not take the effect coefficients from `manifest.json`. It reads a handful of
tables at addresses hardcoded in the C++, and four of those are *pointer tables*: runs of image VAs
it dereferences to reach the reverb tap, level and coefficient rows.

Neither kind survives a re-pack, and a pointer table cannot survive one even in principle -- a VA
encodes an address, so the segment map, which matches content, can never place one. They have to be
found per build and their entries resolved through that build's own section table.

Plain data tables are found by content. Pointer tables are found structurally: scan for runs of
values that are valid VAs in the target, dereference each candidate, and keep the one whose rows
match the reference build's byte for byte. That is decisive, because the rows are the same data in
both builds even though the pointers naming them are not. It also handles a 32-bit target, where the
entries are 4 bytes rather than 8, straight from the PE header.

Usage:
    python3 tools/locate_effect_tables.py REFERENCE.dll TARGET.dll OUT.json
"""
import argparse
import json
import struct
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("reference", help="the build manifest.json's offsets are recorded in")
parser.add_argument("target", help="the build to locate the same tables in")
parser.add_argument("output", help="where to write the offset map")
args = parser.parse_args()

A = open(args.reference, "rb").read()
B = open(args.target, "rb").read()

IB=0x180000000
def pin_off(va): return va-IB-0x1000
def secs(img):
    e=struct.unpack_from('<I',img,0x3c)[0]; opt=e+24
    magic=struct.unpack_from('<H',img,opt)[0]; is64=magic==0x20b
    ib=struct.unpack_from('<Q' if is64 else '<I',img,opt+(24 if is64 else 28))[0]
    nsec=struct.unpack_from('<H',img,e+6)[0]; optsz=struct.unpack_from('<H',img,e+20)[0]
    return ib,[struct.unpack_from('<IIII',img,opt+optsz+40*i+8) for i in range(nsec)],is64
IBB,SB,IS64=secs(B)
PW=8 if IS64 else 4
def b_va_to_off(va):
    rva=va-IBB
    for vsize,vaddr,rsize,raw in SB:
        if vaddr<=rva<vaddr+max(vsize,rsize):
            o=raw+(rva-vaddr)
            return o if o<raw+rsize else None
def b_is_va(v): return IBB<=v<IBB+0x1a40000 and b_va_to_off(v) is not None
def rd(off): return struct.unpack_from('<Q' if PW==8 else '<I',B,off)[0]

lo,hi=min(r[3] for r in SB if r[3]>0), max(r[3]+r[2] for r in SB)
cands=[];i=lo-(lo%PW);run=0
while i<hi-PW:
    if b_is_va(rd(i)): run+=1
    else:
        if run>=8: cands.append(i-run*PW)
        run=0
    i+=PW
if run>=8: cands.append(i-run*PW)

PTR={'tap_pointers':(0x1819A0CC0,24),'second_tap_pointers':(0x1819A0AB0,8),
     'level_pointers':(0x1819A0E90,1),'coefficient_pointers':(0x1819A0EE0,20)}
DATA={'reverb_macro_rows':(0x1819A0248,8*7),'chorus_macro_rows':(0x180093640,8*8),
      'chorus_lpf_ladder':(0x180093680,32),'reverb_damp_ladder':(0x181893738,32),
      'delay_preset_offset':(0x181893930,64),'eq_low_table':(0x1818960B0,64),
      'eq_high_table':(0x1818961E0,64)}
out={}
print('%-22s %-12s %s'%('symbol','pinned off','target offset'))
for name,(va,n) in sorted(DATA.items()):
    po=pin_off(va); w=A[po:po+n]
    hits=[];o=B.find(w)
    while o>=0 and len(hits)<8: hits.append(o); o=B.find(w,o+1)
    # Several tables hold identical bytes -- the chorus LPF and reverb damping ladders are the same
    # 32 bytes as each other, and the target keeps three copies. Any of them reads correctly, so take
    # the lowest deterministically rather than calling an immaterial choice a failure.
    same = all(B[h:h+n]==w for h in hits)
    out[name]=hits[0] if (len(hits)==1 or (hits and same)) else None
    note='' if len(hits)<=1 else '  (%d identical copies; lowest taken)'%len(hits)
    print('%-22s 0x%-10x %s%s'%(name,po,('0x%x'%hits[0]) if hits else 'NOT FOUND',note))
for name,(va,elems) in sorted(PTR.items()):
    po=pin_off(va); N=elems*2
    pin_rows=[A[pin_off(struct.unpack_from('<Q',A,po+8*i)[0]):][:N] for i in range(8)]
    hits=[]
    for c in cands:
        for shift in range(0,64*PW,PW):
            base=c+shift
            if base+8*PW>hi: break
            tv=[rd(base+PW*i) for i in range(8)]
            if not all(b_is_va(v) for v in tv): continue
            if all(B[b_va_to_off(tv[i]):][:N]==pin_rows[i] for i in range(8)): hits.append(base)
    uniq=sorted(set(hits))
    out[name]=uniq[0] if len(uniq)==1 else None
    print('%-22s 0x%-10x %s'%(name,po,('0x%x'%uniq[0]) if len(uniq)==1 else ('%d candidates'%len(uniq) if uniq else 'NOT FOUND')))
print('\nresolved %d of %d'%(sum(1 for v in out.values() if v is not None),len(out)))
json.dump({k:(('0x%x'%v) if v is not None else None) for k,v in out.items()}, open(args.output,'w'), indent=1)
