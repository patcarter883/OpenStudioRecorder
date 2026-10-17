#!/usr/bin/env python3
"""Dump PMT ES entries (stream_type + descriptors) from an MPEG-TS file."""
import sys


def pmt_pid_from_pat(data):
    for i in range(0, len(data) - 188, 188):
        pkt = data[i:i + 188]
        if pkt[0] != 0x47 or not (pkt[1] & 0x40) or not (pkt[3] & 0x10):
            continue
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        if pid != 0:
            continue
        afc = (pkt[3] >> 4) & 0x03
        if not (afc & 0x01):
            continue
        pay = pkt[4:]
        if afc & 0x02:
            pay = pay[1 + pay[0]:]
        sec = pay
        sec = sec[1 + sec[0]:]
        if sec[0] != 0x00:
            continue
        slen = ((sec[1] & 0x0F) << 8) | sec[2]
        end = 3 + slen - 4  # exclude CRC
        off = 8  # after table_id(1) len(2) tsid(2) ver(1) secnum(1) last(1)
        while off + 4 <= end:
            prog = (sec[off] << 8) | sec[off + 1]
            value = ((sec[off + 2] & 0x1F) << 8) | sec[off + 3]
            if prog != 0:
                return value
            off += 4
        return None
    return None


def dump_pmt(data, pmt_pid):
    for i in range(0, len(data) - 188, 188):
        pkt = data[i:i + 188]
        if pkt[0] != 0x47 or not (pkt[1] & 0x40):
            continue
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        if pid != pmt_pid:
            continue
        afc = (pkt[3] >> 4) & 0x03
        if not (afc & 0x01):
            continue
        pay = pkt[4:]
        if afc & 0x02:
            pay = pay[1 + pay[0]:]
        sec = pay
        sec = sec[1 + sec[0]:]
        if sec[0] != 0x02:
            continue
        slen = ((sec[1] & 0x0F) << 8) | sec[2]
        end = 3 + slen - 4  # exclude CRC
        pcr = ((sec[8] & 0x1F) << 8) | sec[9]
        pilen = ((sec[10] & 0x0F) << 8) | sec[11]
        print(f'program: PCR pid {pcr} (0x{pcr:x})')
        off = 12 + pilen
        while off + 5 <= end:
            st = sec[off]
            espid = ((sec[off + 1] & 0x1F) << 8) | sec[off + 2]
            elen = ((sec[off + 3] & 0x0F) << 8) | sec[off + 4]
            desc = sec[off + 5:off + 5 + elen]
            print(f'  stream_type=0x{st:02x} pid={espid} (0x{espid:x}) '
                  f'es_info({elen})={desc.hex()}')
            off += 5 + elen
        return
    print('  (no PMT packet found)')


def main(path):
    data = open(path, 'rb').read()
    pmt = pmt_pid_from_pat(data)
    print(f'PMT pid = {pmt} (0x{pmt:x})' if pmt is not None else 'no PAT found')
    if pmt is not None:
        dump_pmt(data, pmt)


if __name__ == '__main__':
    for p in sys.argv[1:]:
        print(f'== {p} ==')
        main(p)
