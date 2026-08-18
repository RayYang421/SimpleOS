#!/usr/bin/env python3
"""Builds the SD card image the FAT32 exercises run against, and reads it back.

The lab ships a prepared image; this makes an equivalent one from scratch so
the tests have something deterministic to check against, and so the same code
can verify what the kernel wrote:

    tools/make_sdcard.py build sd.img     # partition, format, add FAT_R.TXT
    tools/make_sdcard.py ls sd.img        # list the root directory
    tools/make_sdcard.py cat sd.img NAME  # print one file
    tools/make_sdcard.py check sd.img     # verify the structures are consistent
    tools/make_sdcard.py corrupt sd.img KIND   # build a deliberately broken one
    tools/make_sdcard.py corrupt --list        # what KIND can be

Needs sfdisk and mkfs.vfat, but no root: the partition table and the file
system are written into a plain file, and the one file the kernel expects to
find is placed by hand rather than through a loop mount.
"""

import os
import struct
import subprocess
import sys

IMAGE_MB = 64
PART_LBA = 2048                 # where the lab's image puts the partition too
SECTOR = 512

READ_FILE = "FAT_R.TXT"
READ_TEXT = b"Hello from the SD card, read through FAT32.\n"

ATTR_DIRECTORY = 0x10
ATTR_VOLUME_ID = 0x08
ATTR_LFN = 0x0F
EOC = 0x0FFFFFFF


class Fat32:
    """Just enough FAT32 to place a file and to read the directory back."""

    def __init__(self, path, writable=False):
        self.path = path
        self.f = open(path, "r+b" if writable else "rb")

        mbr = self.read_sector(0)
        if mbr[510] != 0x55 or mbr[511] != 0xAA:
            raise SystemExit(f"{path}: no partition table")

        entry = mbr[0x1BE:0x1BE + 16]
        if entry[4] not in (0x0B, 0x0C):
            raise SystemExit(f"{path}: first partition is not FAT32")

        self.part_lba = struct.unpack_from("<I", entry, 8)[0]

        b = self.read_sector(self.part_lba)
        self.bytes_per_sector = struct.unpack_from("<H", b, 0x0B)[0]
        self.sectors_per_cluster = b[0x0D]
        reserved = struct.unpack_from("<H", b, 0x0E)[0]
        self.num_fats = b[0x10]
        self.fat_sectors = struct.unpack_from("<I", b, 0x24)[0]
        self.root_cluster = struct.unpack_from("<I", b, 0x2C)[0]

        self.fat_start = self.part_lba + reserved
        self.data_start = self.fat_start + self.num_fats * self.fat_sectors

    def read_sector(self, lba):
        self.f.seek(lba * SECTOR)
        return bytearray(self.f.read(SECTOR))

    def write_sector(self, lba, data):
        assert len(data) == SECTOR
        self.f.seek(lba * SECTOR)
        self.f.write(data)

    def cluster_sector(self, cluster):
        return self.data_start + (cluster - 2) * self.sectors_per_cluster

    def fat_entry(self, cluster):
        off = cluster * 4
        sector = self.read_sector(self.fat_start + off // SECTOR)
        return struct.unpack_from("<I", sector, off % SECTOR)[0] & 0x0FFFFFFF

    def set_fat_entry(self, cluster, value):
        off = cluster * 4
        # Both copies of the table, or a host fsck will call the image corrupt.
        for copy in range(self.num_fats):
            lba = self.fat_start + copy * self.fat_sectors + off // SECTOR
            sector = self.read_sector(lba)
            struct.pack_into("<I", sector, off % SECTOR, value & 0x0FFFFFFF)
            self.write_sector(lba, sector)

    def find_free_cluster(self):
        for cluster in range(2, 2 + self.fat_sectors * SECTOR // 4):
            if self.fat_entry(cluster) == 0:
                return cluster
        raise SystemExit("no free cluster")

    def chain(self, cluster):
        while 2 <= cluster < 0x0FFFFFF8:
            yield cluster
            cluster = self.fat_entry(cluster)

    @staticmethod
    def short_name(name):
        """"FAT_R.TXT" -> the 11-byte padded on-disk form."""
        stem, _, ext = name.upper().partition(".")
        return (stem[:8].ljust(8) + ext[:3].ljust(3)).encode()

    @staticmethod
    def display_name(raw):
        stem = raw[:8].decode(errors="replace").rstrip()
        ext = raw[8:11].decode(errors="replace").rstrip()
        return f"{stem}.{ext}" if ext else stem

    def dir_entries(self, cluster):
        """Yields (lba, offset, raw) for every used entry in a directory."""
        for cl in self.chain(cluster):
            for i in range(self.sectors_per_cluster):
                lba = self.cluster_sector(cl) + i
                sector = self.read_sector(lba)
                for off in range(0, SECTOR, 32):
                    raw = sector[off:off + 32]
                    if raw[0] == 0x00:
                        return
                    if raw[0] == 0xE5 or raw[11] in (ATTR_LFN, ATTR_VOLUME_ID):
                        continue
                    yield lba, off, raw

    def listing(self):
        out = {}
        for _, _, raw in self.dir_entries(self.root_cluster):
            first = (struct.unpack_from("<H", raw, 20)[0] << 16) | \
                    struct.unpack_from("<H", raw, 26)[0]
            size = struct.unpack_from("<I", raw, 28)[0]
            out[self.display_name(raw)] = (first, size)
        return out

    def read_file(self, name):
        for display, (first, size) in self.listing().items():
            if display != name.upper():
                continue

            data = bytearray()
            for cl in self.chain(first):
                for i in range(self.sectors_per_cluster):
                    data += self.read_sector(self.cluster_sector(cl) + i)
            return bytes(data[:size])
        return None

    def free_dir_slot(self, cluster):
        for cl in self.chain(cluster):
            for i in range(self.sectors_per_cluster):
                lba = self.cluster_sector(cl) + i
                sector = self.read_sector(lba)
                for off in range(0, SECTOR, 32):
                    if sector[off] in (0x00, 0xE5):
                        return lba, off
        raise SystemExit("the root directory is full")

    def add_file(self, name, content):
        cluster = self.find_free_cluster()
        self.set_fat_entry(cluster, EOC)

        per_cluster = self.sectors_per_cluster * SECTOR
        if len(content) > per_cluster:
            raise SystemExit("this helper only writes single-cluster files")

        padded = content + bytes(per_cluster - len(content))
        for i in range(self.sectors_per_cluster):
            self.write_sector(self.cluster_sector(cluster) + i,
                              padded[i * SECTOR:(i + 1) * SECTOR])

        lba, off = self.free_dir_slot(self.root_cluster)
        sector = self.read_sector(lba)

        entry = bytearray(32)
        entry[0:11] = self.short_name(name)
        entry[11] = 0x20                                # archive
        struct.pack_into("<H", entry, 20, cluster >> 16)
        struct.pack_into("<H", entry, 26, cluster & 0xFFFF)
        struct.pack_into("<I", entry, 28, len(content))

        sector[off:off + 32] = entry
        self.write_sector(lba, sector)

    def check(self):
        """Structural problems a kernel writing FAT32 badly would leave behind.

        Returns a list of complaints, empty if the volume is consistent."""
        problems = []

        # Every copy of the table has to say the same thing, or a host will
        # call the volume corrupt however well the first copy reads.
        for sector in range(self.fat_sectors):
            first = None
            for copy in range(self.num_fats):
                data = self.read_sector(self.fat_start +
                                        copy * self.fat_sectors + sector)
                if first is None:
                    first = data
                elif data != first:
                    problems.append(
                        f"FAT copies differ at table sector {sector}")
                    break

        seen = set()
        for name, (first, size) in self.listing().items():
            if first == 0:
                if size:
                    problems.append(f"{name}: {size} bytes but no cluster")
                continue

            chain = []
            cluster = first
            while 2 <= cluster < 0x0FFFFFF8:
                if cluster in chain:
                    problems.append(f"{name}: its cluster chain loops")
                    break
                if cluster in seen:
                    problems.append(f"{name}: shares cluster {cluster}")
                chain.append(cluster)
                seen.add(cluster)
                cluster = self.fat_entry(cluster)
            else:
                if cluster < 0x0FFFFFF8:
                    problems.append(f"{name}: chain ends at {cluster:#x}")

            held = len(chain) * self.sectors_per_cluster * SECTOR
            if size > held:
                problems.append(
                    f"{name}: {size} bytes in {held} bytes of clusters")

        return problems

    def close(self):
        self.f.close()


# --- deliberately broken volumes ---------------------------------------------
#
# Each of these is something a kernel that trusts the card would follow off the
# end of the volume, or into a loop it never leaves. They exist so that
# refusing them can be demonstrated rather than asserted.

CORRUPTIONS = {
    "num-fats":
        "the boot sector claims zero allocation tables, so nothing would "
        "record a cluster as used",
    "total-sectors":
        "the volume is smaller than its own metadata, which underflows the "
        "cluster count to about four billion",
    "cluster-size":
        "sectors per cluster is 3, which is not a power of two",
    "root-cluster":
        "the root directory starts past the end of the volume",
    "first-cluster":
        "a file's first cluster is far outside the volume, so its sector "
        "number overflows into the metadata",
    "cyclic-chain":
        "the root directory is full and its cluster chain points at itself",
}


def corrupt(path, kind):
    """Builds a good volume and then breaks it one specific way."""
    build(path, quiet=True)

    fs = Fat32(path, writable=True)
    boot = fs.read_sector(fs.part_lba)

    if kind == "num-fats":
        boot[0x10] = 0
        fs.write_sector(fs.part_lba, boot)

    elif kind == "total-sectors":
        # Fewer sectors than the reserved region and the tables occupy.
        struct.pack_into("<I", boot, 0x20, 8)
        fs.write_sector(fs.part_lba, boot)

    elif kind == "cluster-size":
        boot[0x0D] = 3
        fs.write_sector(fs.part_lba, boot)

    elif kind == "root-cluster":
        struct.pack_into("<I", boot, 0x2C, 0x00FF0000)
        fs.write_sector(fs.part_lba, boot)

    elif kind == "first-cluster":
        # Large enough that data_start + (cluster - 2) * spc leaves 32 bits.
        for lba, off, raw in fs.dir_entries(fs.root_cluster):
            if fs.display_name(raw) != READ_FILE:
                continue
            sector = fs.read_sector(lba)
            struct.pack_into("<H", sector, off + 20, 0x0FFF)
            struct.pack_into("<H", sector, off + 26, 0xFFF0)
            fs.write_sector(lba, sector)
            break

    elif kind == "cyclic-chain":
        # A cycle only bites where something walks a chain to its end, which is
        # the search for a free directory entry. So the directory has to be
        # full -- no entry free or deleted anywhere in it -- and the chain has
        # to lead back to the start.
        #
        # Every entry is a long-name fragment, which a reader skips without
        # counting and which is neither free nor deleted. So nothing stops the
        # walk: no entry ends the directory, no name fills the entry cache, and
        # the chain never reaches an end. A reader that does not count its own
        # steps goes round for ever.
        sector = bytearray()
        for i in range(SECTOR // 32):
            entry = bytearray(32)
            entry[0] = 0x41 + i                 # a sequence number, not free
            entry[11] = ATTR_LFN
            sector += entry

        fs.write_sector(fs.cluster_sector(fs.root_cluster), sector)
        for i in range(1, fs.sectors_per_cluster):
            fs.write_sector(fs.cluster_sector(fs.root_cluster) + i, sector)

        fs.set_fat_entry(fs.root_cluster, fs.root_cluster)

    else:
        fs.close()
        raise SystemExit(f"unknown corruption {kind!r}")

    fs.close()
    print(f"{path}: {kind} -- {CORRUPTIONS[kind]}")


def build(path, quiet=False):
    if os.path.exists(path):
        os.remove(path)

    with open(path, "wb") as f:
        f.truncate(IMAGE_MB * 1024 * 1024)

    layout = f"label: dos\nstart={PART_LBA}, type=c\n"
    subprocess.run(["sfdisk", path], input=layout.encode(),
                   check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    subprocess.run(["mkfs.vfat", "-F", "32", "-S", str(SECTOR),
                    "-n", "OSCSD", "--offset", str(PART_LBA), path],
                   check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)

    fs = Fat32(path, writable=True)
    fs.add_file(READ_FILE, READ_TEXT)
    fs.close()

    if not quiet:
        print(f"{path}: FAT32 partition at block {PART_LBA}, "
              f"containing {READ_FILE}")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    action, path = sys.argv[1], sys.argv[2]

    if action == "build":
        build(path)

    elif action == "ls":
        fs = Fat32(path)
        for name, (cluster, size) in sorted(fs.listing().items()):
            print(f"{name:14} cluster {cluster:<6} {size} bytes")
        fs.close()

    elif action == "corrupt":
        if path == "--list":
            for kind, why in CORRUPTIONS.items():
                print(f"{kind:16} {why}")
            return
        corrupt(path, sys.argv[3])

    elif action == "check":
        fs = Fat32(path)
        problems = fs.check()
        fs.close()
        for p in problems:
            print(p)
        raise SystemExit(1 if problems else 0)

    elif action == "cat":
        fs = Fat32(path)
        data = fs.read_file(sys.argv[3])
        fs.close()
        if data is None:
            raise SystemExit(f"{sys.argv[3]}: not found")
        sys.stdout.write(data.decode(errors="replace"))

    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
