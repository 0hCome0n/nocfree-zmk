import struct, sys, glob
MAGIC0, MAGIC1, MAGIC2 = 0x0A324655, 0x9E5D5157, 0x0AB16F30
for path in sorted(sys.argv[1:]):
    d = open(path,'rb').read()
    if len(d) % 512:
        print(f"{path}: NOT a uf2 (size {len(d)} not a multiple of 512)"); continue
    addrs, fams, bad = [], set(), 0
    n = len(d)//512
    for i in range(n):
        b = d[i*512:(i+1)*512]
        m0,m1,flags,addr,plen,blkno,nblk,famid = struct.unpack('<8I', b[:32])
        if m0!=MAGIC0 or m1!=MAGIC1 or struct.unpack('<I',b[-4:])[0]!=MAGIC2:
            bad += 1; continue
        addrs.append(addr); fams.add(famid)
    lo, hi = min(addrs), max(addrs)+256
    fam = ", ".join(f"{f:#010x}" for f in fams)
    print(f"{path.split('/')[-1]}")
    print(f"   blocks {n}  bad {bad}  family {fam}")
    print(f"   flash 0x{lo:05x} .. 0x{hi:05x}   ({hi-lo} bytes)")
