"""Extracts the .plugin bundle from the macOS installer (.pkg) and zips it.

The zip is what people copy to their plugins folder by hand and what the plugin's
updater downloads on macOS (ac-motion-mask-<version>-macos.zip).

Usage: python3 pkg-to-plugin-zip.py <installer.pkg> <output.zip>
"""

import gzip
import os
import stat
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile
import zlib


def payload(pkg_bytes):
    _, header_size, _, toc_size, _, _ = struct.unpack(">4sHHQQI", pkg_bytes[:28])
    heap = header_size + toc_size
    toc = ET.fromstring(zlib.decompress(pkg_bytes[header_size:heap]))

    def walk(element):
        for f in element.findall("file"):
            if f.find("name").text == "Payload":
                data = f.find("data")
                offset = int(data.find("offset").text)
                length = int(data.find("length").text)
                return pkg_bytes[heap + offset : heap + offset + length]
            found = walk(f)
            if found is not None:
                return found
        return None

    return gzip.decompress(walk(toc.find("toc")))


def entries(cpio):
    i = 0
    while True:
        if cpio[i : i + 6] != b"070707":
            raise ValueError("unexpected cpio format")
        mode = int(cpio[i + 18 : i + 24], 8)
        name_len = int(cpio[i + 59 : i + 65], 8)
        size = int(cpio[i + 65 : i + 76], 8)
        name = cpio[i + 76 : i + 76 + name_len - 1].decode()
        i += 76 + name_len
        data = cpio[i : i + size]
        i += size
        if name == "TRAILER!!!":
            return
        yield name, mode, data


def main(pkg_path, zip_path):
    cpio = payload(open(pkg_path, "rb").read())
    written = 0
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as out:
        for name, mode, data in entries(cpio):
            marker = ".plugin/"
            index = name.find(marker)
            if index < 0 or not stat.S_ISREG(mode):
                continue
            start = name.rfind("/", 0, index) + 1
            arcname = os.path.normpath(name[start:])
            if arcname.startswith(".."):
                continue
            info = zipfile.ZipInfo(arcname)
            info.external_attr = (stat.S_IFREG | (mode & 0o777)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            out.writestr(info, data)
            written += 1
    if not written:
        sys.exit("no .plugin bundle found in " + pkg_path)
    print(f"{zip_path}: {written} files")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
