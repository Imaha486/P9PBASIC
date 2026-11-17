#!/usr/bin/env python3
"""Extract files from a PC-88 N88 Disk BASIC (2D) D88 image.
Format (reverse-engineered from MA2026A.D88, 2026-11-17):
- D88 media 0x00 (2D): 40 cyl x 2 heads x 16 sec x 256 B = 327,680 B raw.
- Directory: C18 H1 R1, 16-byte entries [name 6][ext 3][attr 1][start cluster 1][0xFF x5];
  first byte 0x00 = deleted, 0xFF = end/free.
- FAT: 3 identical copies at C18 H1 R14/R15/R16 (256 B each).
  One byte per cluster: 0x00-0x9F next, 0xC1-0xDA last (low5 = sectors used), 0xFE reserved, 0xFF free.
- 1 cluster = 8 sectors (2048 B), numbered in (C,H,R) sector order over the whole disk.
"""
import struct, sys, os, json

def load_sectors(path):
    d = open(path, 'rb').read()
    offs = struct.unpack('<164I', d[0x20:0x20+164*4])
    secs = {}
    for off in offs:
        if not off: continue
        p = off
        cnt = struct.unpack('<H', d[p+4:p+6])[0]
        for _ in range(cnt):
            c,h,r = d[p], d[p+1], d[p+2]
            ds = struct.unpack('<H', d[p+14:p+16])[0]
            secs[(c,h,r)] = d[p+16:p+16+ds]
            p += 16 + ds
    return secs

def sector_stream(secs):
    out = []
    for c in range(40):
        for h in range(2):
            for r in range(1,17):
                out.append(secs.get((c,h,r), b'\x00'*256))
    return out  # index = global sector number = cluster*8 + pos

def main():
    path, outdir = sys.argv[1], sys.argv[2]
    secs = load_sectors(path)
    stream = sector_stream(secs)
    dsec = secs[(18,1,1)]
    fat_copies = [secs[(18,1,r)] for r in (14,15,16)]
    assert fat_copies[0] == fat_copies[1] == fat_copies[2], "FAT copies differ"
    fat = fat_copies[0]
    os.makedirs(outdir, exist_ok=True)
    meta = []
    for i in range(len(dsec)//16):
        e = dsec[i*16:(i+1)*16]
        if e == b'\xff'*16 or e[0] == 0x00: continue
        name = e[0:6].decode('shift_jis','replace').rstrip()
        ext = e[6:9].decode('shift_jis','replace').rstrip()
        attr, start = e[9], e[10]
        clusters, cur = [], start
        while True:
            clusters.append(cur)
            v = fat[cur]
            if 0xC1 <= v <= 0xDA:
                last_bytes = (v & 0x1F) * 256
                break
            cur = v
        data = b''
        for cl in clusters[:-1]:
            data += b''.join(stream[cl*8+j] for j in range(8))
        data += b''.join(stream[clusters[-1]*8+j] for j in range(8))[:last_bytes]
        fname = (name + ('.'+ext if ext else '')).replace('/','_')
        open(os.path.join(outdir, fname), 'wb').write(data)
        meta.append({'file': fname, 'attr': attr, 'start': start, 'clusters': clusters, 'bytes': len(data)})
        print(f"{fname:16s} attr={attr:#04x} start={start} clusters={clusters} bytes={len(data)}")
    json.dump(meta, open(os.path.join(outdir,'_meta.json'),'w'), indent=1)

if __name__ == '__main__':
    main()
