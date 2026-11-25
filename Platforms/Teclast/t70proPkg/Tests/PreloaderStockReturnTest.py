#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Ensure partial-command recovery never sends loader bytes before exact replies."""
from pathlib import Path
import functools
import runpy
import struct

finish=runpy.run_path(str(Path(__file__).parents[1]/'Tools/preloader-stock-return.py'))['finish_command']
data=bytes(range(256))*(427912//256)+bytes(range(427912%256))
checksum=functools.reduce(lambda a,b:a^b,struct.unpack('<'+str(len(data)//2)+'H',data),0)
valid=[struct.pack('>I',len(data)),struct.pack('>I',256),b'\0\0',struct.pack('>HH',checksum,0)]
for name,replies,allowed in [('valid',valid,True),('valid_split_final_reply',valid[:3]+[valid[3][:2],valid[3][2:]],True),
                            ('valid_split_echo',[valid[0][:2],valid[0][2:]]+valid[1:],True),
                            ('wrong_size_echo',[b'bad!'],False),
                            ('wrong_signature_echo',[valid[0],b'bad!'],False),
                            ('idle_loop_no_status',valid[:2]+[b''],False),
                            ('rejected_status',valid[:2]+[b'\x10\x00'],False)]:
    queue=list(replies);sent=[]
    def write(b):sent.append(bytes(b));return len(b)
    def read(n):return queue.pop(0)
    try:finish(write,read,data)
    except ValueError:
        assert not allowed,name
        assert all(len(b)<=4 for b in sent),name
    else:
        assert allowed,name
        assert b''.join(b for b in sent[2:] if b)==data
print('Passed 7 transport gating cases; no device access')
