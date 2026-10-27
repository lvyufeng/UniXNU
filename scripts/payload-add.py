#!/usr/bin/env python3
"""Add files to the payload FAT32 image without rebuilding it.

Rebuilding the payload needs the platform seed, which is not kept in any repository. Adding a
file to the image that already exists needs none: the directory entries and the FAT are plain
structure, and a read-only-at-runtime image is still writable here.

Only what this platform needs is implemented: creating files in an existing directory, growing
that directory when it has no free entry, and chaining clusters for a file longer than one.
"""

import struct
import sys
from pathlib import Path

FAT_EOC = 0x0FFFFFFF


class Fat:
    def __init__(self, path):
        self.path = Path(path)
        self.data = bytearray(self.path.read_bytes())
        d = self.data
        self.bps = struct.unpack_from('<H', d, 11)[0]
        self.spc = d[13]
        self.rsv = struct.unpack_from('<H', d, 14)[0]
        self.nfat = d[16]
        self.spf = struct.unpack_from('<I', d, 36)[0]
        self.root = struct.unpack_from('<I', d, 44)[0]
        self.fat_off = self.rsv * self.bps
        self.data_off = (self.rsv + self.nfat * self.spf) * self.bps
        self.cluster_bytes = self.bps * self.spc
        self.max_cluster = (len(d) - self.data_off) // self.cluster_bytes + 2

    # ---- FAT table -------------------------------------------------------

    def fat_get(self, n):
        return struct.unpack_from('<I', self.data, self.fat_off + n * 4)[0] & 0x0FFFFFFF

    def fat_set(self, n, value):
        for copy in range(self.nfat):
            off = self.fat_off + copy * self.spf * self.bps + n * 4
            old = struct.unpack_from('<I', self.data, off)[0]
            struct.pack_into('<I', self.data, off, (old & 0xF0000000) | (value & 0x0FFFFFFF))

    def allocate(self, count):
        """Take `count` free clusters and chain them; returns the first."""
        free = [n for n in range(2, self.max_cluster) if self.fat_get(n) == 0][:count]
        if len(free) < count:
            raise SystemExit(f'not enough free clusters: want {count}, have {len(free)}')
        for a, b in zip(free, free[1:]):
            self.fat_set(a, b)
        self.fat_set(free[-1], FAT_EOC)
        for n in free:
            off = self.cluster_offset(n)
            self.data[off:off + self.cluster_bytes] = b'\0' * self.cluster_bytes
        return free[0]

    def cluster_offset(self, n):
        return self.data_off + (n - 2) * self.cluster_bytes

    # ---- directories -----------------------------------------------------

    def read_dir(self, cluster):
        return self.data[self.cluster_offset(cluster):self.cluster_offset(cluster) + self.cluster_bytes]

    def dir_entries(self, cluster):
        raw = self.read_dir(cluster)
        out = []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            out.append((i, e))
        return out

    def find(self, cluster, name):
        """Find a child by its long name, falling back to the short one."""
        lfn = ''
        for off, e in self.dir_entries(cluster):
            attr = e[11]
            if attr == 0x0F:
                lfn = e
                continue
            short = e[0:11].decode('ascii', 'replace').replace(' ', '')
            target = name.upper().replace('.', '').replace('-', '')
            if short == target or (lfn and lfn[0] != 0 and self.lfn_name(e, lfn) == name.upper()):
                return off, e
            lfn = ''
        return None, None

    @staticmethod
    def lfn_name(short_entry, lfn_entry):
        chars = []
        for i in [1, 3, 5, 7, 9]:
            chars.append(struct.unpack_from('<H', lfn_entry, i)[0])
        for i in [14, 16, 18]:
            chars.append(struct.unpack_from('<H', lfn_entry, i)[0])
        for i in [20, 22, 24]:
            chars.append(struct.unpack_from('<H', lfn_entry, i)[0])
        for i in [26, 28, 30]:
            chars.append(struct.unpack_from('<H', lfn_entry, i)[0])
        return ''.join(chr(c) for c in chars if c not in (0, 0xFFFF))

    def walk(self, path):
        """Return (directory cluster, offset of the entry) for a path under the root."""
        parts = [p for p in path.replace('\\', '/').split('/') if p]
        cluster = self.root
        for part in parts[:-1]:
            off, entry = self.find(cluster, part.upper())
            if entry is None:
                raise SystemExit(f'no such directory component: {part}')
            cluster = (struct.unpack_from('<H', entry, 20)[0] << 16) | struct.unpack_from('<H', entry, 26)[0]
        return cluster, parts[-1]

    # ---- writing ---------------------------------------------------------

    def link(self, path, target_path):
        """Give an existing file a second name, pointing at the same first cluster."""
        cluster, name = self.walk(path)
        _off, source = self.find(cluster, target_path.upper())
        if source is None:
            raise SystemExit(f'no such file to alias: {target_path}')
        entry_off = self._free_entry(cluster, (len(name) + 12) // 13)
        e = bytearray(source)
        self.data[entry_off:entry_off + 32] = e
        short = self._short_name(name, cluster, entry_off)
        self.data[entry_off:entry_off + 11] = short
        if self._needs_lfn(name, short):
            self._write_lfn(cluster, entry_off, name, short)
        return name

    def add(self, path, content):
        cluster, name = self.walk(path)
        existing_off, existing = self.find(cluster, name.upper())
        if existing is not None:
            old_first = (struct.unpack_from('<H', existing, 20)[0] << 16) | struct.unpack_from('<H', existing, 26)[0]
            self._free_chain(old_first)
            entry_off = self.cluster_offset(cluster) + existing_off
        else:
            pieces = 0 if not self._needs_lfn(name, b'X' * 11) else (len(name) + 12) // 13
            entry_off = self._free_entry(cluster, pieces)

        need = max(1, (len(content) + self.cluster_bytes - 1) // self.cluster_bytes)
        first = self.allocate(need)

        chain = [first]
        for _ in range(need - 1):
            chain.append(self.fat_get(chain[-1]))
        for index, c in enumerate(chain):
            chunk = content[index * self.cluster_bytes:(index + 1) * self.cluster_bytes]
            off = self.cluster_offset(c)
            self.data[off:off + len(chunk)] = chunk

        short = self._short_name(name, cluster, existing_off)
        e = bytearray(32)
        e[0:11] = short
        e[11] = 0x20
        struct.pack_into('<H', e, 20, (first >> 16) & 0xFFFF)
        struct.pack_into('<H', e, 26, first & 0xFFFF)
        struct.pack_into('<I', e, 28, len(content))
        self.data[entry_off:entry_off + 32] = e

        #
        # The long name goes just before the entry.
        #
        # Without it only the 8.3 form exists, and anything that opens the file by the name it
        # was given - LoadImage does, from the device path - finds nothing: the driver load
        # failed with EFI_NOT_FOUND on a file whose bytes were verifiably correct in the image.
        #
        if self._needs_lfn(name, short):
            self._write_lfn(cluster, entry_off, name, short)
        return first

    @staticmethod
    def _needs_lfn(name, short):
        base, dot, ext = name.upper().partition('.')
        plain = (base + dot + ext).replace('.', '')
        return len(base) > 8 or len(ext) > 3 or base.upper() != base or '~' in base

    def _short_name(self, name, cluster, entry_off):
        base, _, ext = name.upper().partition('.')
        allowed = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_^$~!#%&-{}()@`')
        safe = ''.join(c for c in base if c in allowed)[:8] or 'FILE'
        safe_ext = ''.join(c for c in ext if c in allowed)[:3]
        fits = len(base) <= 8 and len(ext) <= 3 and len(safe) == len(base) and len(safe_ext) == len(ext)
        if fits and ' ' not in name:
            return (safe.ljust(8) + safe_ext.ljust(3)).encode('ascii')
        for n in range(1, 100):
            tail = f'~{n}'
            candidate = (safe[:8 - len(tail)] + tail).ljust(8) + safe_ext.ljust(3)
            taken = any(
                bytes(e[0:11]) == candidate.encode('ascii')
                for _, e in self.dir_entries(cluster)
                if e[11] != 0x0F
            )
            if not taken:
                return candidate.encode('ascii')
        raise SystemExit(f'no free short name for {name}')

    def _write_lfn(self, cluster, entry_off, name, short):
        chars = list(name)
        while len(chars) % 13:
            chars.append('\0')
        pieces = [chars[i:i + 13] for i in range(0, len(chars), 13)]
        checksum = 0
        for b in short:
            checksum = ((checksum & 1) << 7) + (checksum >> 1) + b
            checksum &= 0xFF

        start = entry_off
        for index in range(len(pieces) - 1, -1, -1):
            start -= 32
            if start < self.cluster_offset(cluster):
                raise SystemExit('no room for the long name before the entry')
            #
            # The entry with the highest characters comes first on disk and carries 0x40; the
            # one with the first characters comes last and is numbered 1. Writing them in
            # ascending order puts the tail where the head belongs, which is what happened -
            # the name read back as "DarwinBoot.ef" + "i" with the numbers swapped.
            #
            seq = index + 1
            if index == len(pieces) - 1:
                seq = (len(pieces)) | 0x40
            e = bytearray(32)
            e[0] = seq
            e[11] = 0x0F
            e[12] = 0
            e[13] = checksum
            struct.pack_into('<H', e, 26, 0)
            group = pieces[index]
            for i in range(5):
                struct.pack_into('<H', e, 1 + i * 2, ord(group[i]))
            for i in range(6):
                struct.pack_into('<H', e, 14 + i * 2, ord(group[5 + i]))
            #
            # Characters 11 and 12 go at offsets 26 and 28. Writing them at 28 and 30 left a
            # zero at 26, which a reader takes as the end of the name - the long name came back
            # as "DarwinBoot.e" and the entry never matched.
            #
            for i in range(2):
                struct.pack_into('<H', e, 26 + i * 2, ord(group[11 + i]))
            self.data[start:start + 32] = e

    def _free_chain(self, first):
        n = first
        while 2 <= n < 0x0FFFFFF7:
            nxt = self.fat_get(n)
            self.fat_set(n, 0)
            n = nxt

    def _free_entry(self, cluster, need=1):
        """First run of `need` free slots.

        The long name occupies slots before the entry, so the run has to be that long: taking
        the first free slot and writing backwards over whatever preceded it silently destroyed
        the entry ahead - the 56 MB kernel vanished from the volume that way, and the boot then
        failed with NOT_FOUND on a file whose name had been fine all along.
        """
        raw = self.read_dir(cluster)
        run = 0
        start = None
        for i in range(0, len(raw), 32):
            if raw[i] in (0x00, 0xE5):
                if run == 0:
                    start = i
                run += 1
                if run >= need + 1:
                    # The entry goes last, in the final slot of the run: the long-name slots
                    # are written backwards from it, so returning the first slot of the run
                    # made them land on whatever preceded it.
                    return self.cluster_offset(cluster) + start + (run - 1) * 32
            else:
                run = 0
                start = None
        raise SystemExit('directory has no run of free entries (extending it is not implemented)')

    def save(self):
        self.path.write_bytes(self.data)


def main():
    if len(sys.argv) < 4:
        raise SystemExit('usage: payload-add.py IMAGE PATH-on-volume FILE')
    image, target, source = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    fat = Fat(image)
    first = fat.add(target, source.read_bytes())
    fat.save()
    print(f'{source.name} -> {target} (first cluster {first}, {source.stat().st_size} bytes)')


if __name__ == '__main__':
    main()