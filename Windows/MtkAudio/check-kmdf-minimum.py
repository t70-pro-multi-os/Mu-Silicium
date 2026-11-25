#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Read-only standard COFF + linked PE/map minimum-version audit. Not runtime proof."""
import argparse,json,re,struct
from pathlib import Path
if not __debug__:
 raise RuntimeError("Run this audit without Python optimization")
p=argparse.ArgumentParser();p.add_argument('objects',type=Path);p.add_argument('--pe',type=Path);p.add_argument('--map',type=Path);p.add_argument('--expected',type=int,default=33);a=p.parse_args()
def u16(b,o):return struct.unpack_from('<H',b,o)[0]
def u32(b,o):return struct.unpack_from('<I',b,o)[0]
def sections(b,o,n):
 return [(b[o+i*40:o+i*40+8].rstrip(b'\0').decode(),*struct.unpack_from('<IIII',b,o+i*40+8)) for i in range(n)]
rows=[]
for path in sorted(a.objects.rglob('*.obj')):
 b=path.read_bytes();machine,n,_,sp,ns,opt,_=struct.unpack_from('<HHIIIHH',b,0)
 if machine!=0xaa64 or opt:raise ValueError(f'unsupported/non-ARM64 COFF: {path}')
 sec=sections(b,20,n);st=sp+ns*18
 if st+4>len(b):raise ValueError(f'invalid symbol table: {path}')
 strings=b[st:st+u32(b,st)];i=0;found=[]
 while i<ns:
  off=sp+i*18;name=b[off:off+8];value,sn,typ,cls,aux=struct.unpack_from('<IhHBB',b,off+8)
  if name[:4]==b'\0'*4:
   ix=u32(name,4);name=strings[ix:].split(b'\0',1)[0]
  else:name=name.rstrip(b'\0')
  if name==b'WdfMinimumVersionRequired' and sn>0:
   snam,vs,va,sz,raw=sec[sn-1]
   if value+4>sz:raise ValueError(f'symbol outside data: {path}')
   got=u32(b,raw+value);assert got==a.expected,(str(path),got);found.append(got)
  i+=1+aux
 if found:rows.append({'object':str(path),'minimums':found})
assert rows,'No WDF minimum definitions found'
result={'expected':a.expected,'wdf_objects':rows,'note':'Compile/link metadata only; API availability and OS loading are not established'}
if a.pe or a.map:
 assert a.pe and a.map,'Both --pe and --map required'
 b=a.pe.read_bytes();pe=u32(b,0x3c);assert b[pe:pe+4]==b'PE\0\0';co=pe+4;assert u16(b,co)==0xaa64;n=u16(b,co+2);os=u16(b,co+16);op=co+20;assert u16(b,op)==0x20b
 base=struct.unpack_from('<Q',b,op+24)[0];sec=sections(b,op+os,n)
 hits=[]
 for line in a.map.read_text().splitlines():
  m=re.search(r'^\s*[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+WdfMinimumVersionRequired\s+([0-9A-Fa-f]+)\b',line)
  if m:hits.append(int(m.group(1),16))
 assert len(set(hits))==1,('missing/ambiguous map symbol',hits)
 rva=hits[0]-base;matches=[(raw+rva-va) for _,vs,va,sz,raw in sec if va<=rva and rva+4<=va+sz];assert len(matches)==1
 got=u32(b,matches[0]);assert got==a.expected,got
 result['linked_minimum']={'va':hex(hits[0]),'rva':hex(rva),'value':got}
print(json.dumps(result,indent=2))
