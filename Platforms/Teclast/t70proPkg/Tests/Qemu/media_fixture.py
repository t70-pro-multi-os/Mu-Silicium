# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build a project-owned FAT12 USB image; no host block device is opened."""
import hashlib, struct

def make_media(efi, target):
    shell=efi.read_bytes()
    assert len(shell)<(4096-61)*512
    disk=bytearray(4096*512);disk[:11]=b"\xeb\x3c\x90T70TEST "
    struct.pack_into("<HBHBHHBHHHII",disk,11,512,1,1,2,512,4096,0xf8,12,18,2,0,0)
    disk[36:62]=bytes.fromhex("80 00 29 12 34 56 78")+b"T70VMTEST  "+b"FAT12   "
    disk[510:512]=b"\x55\xaa";fat=bytearray(12*512)
    def entry(cluster,value):
        off=cluster*3//2;old=int.from_bytes(fat[off:off+2],"little")
        old=(old&0xf)|(value<<4) if cluster&1 else (old&0xf000)|value
        fat[off:off+2]=old.to_bytes(2,"little")
    for c,v in ((0,0xff8),(1,0xfff),(2,0xfff),(3,0xfff)):entry(c,v)
    count=(len(shell)+511)//512
    for i in range(count):entry(4+i,0xfff if i==count-1 else 5+i)
    for off in (512,13*512):disk[off:off+len(fat)]=fat
    def dirent(name,cluster,size=0,directory=False):
        d=bytearray(32);d[:11]=name;d[11]=0x10 if directory else 0x20
        struct.pack_into("<H",d,26,cluster);struct.pack_into("<I",d,28,size);return d
    disk[25*512:25*512+32]=dirent(b"EFI        ",2,directory=True)
    for c,parent,name,child,size,isdir in ((2,0,b"BOOT       ",3,0,True),(3,2,b"BOOTAA64EFI",4,len(shell),False)):
        off=57*512+(c-2)*512
        disk[off:off+32]=dirent(b".          ",c,directory=True)
        disk[off+32:off+64]=dirent(b"..         ",parent,directory=True)
        disk[off+64:off+96]=dirent(name,child,size,isdir)
    disk[59*512:59*512+len(shell)]=shell
    assert len(disk)==4096*512
    target.write_bytes(disk)
    return dict(path=str(target),bytes=len(disk),sha256=hashlib.sha256(disk).hexdigest(),
                boot_file="\\EFI\\BOOT\\BOOTAA64.EFI",boot_file_bytes=len(shell),
                boot_file_sha256=hashlib.sha256(shell).hexdigest(),qemu_readonly=True)
