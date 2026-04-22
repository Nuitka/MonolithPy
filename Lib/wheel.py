import os
import sys
import sysconfig
import importlib.machinery

# Make the standard wheel, the real wheel module.
# Need to keep a reference alive, or the module will loose all attributes.
if "wheel" in sys.modules:
    _this_module = sys.modules["wheel"]
    del sys.modules["wheel"]

loader = importlib.machinery.SourceFileLoader('wheel', os.path.join(os.path.dirname(__file__), "site-packages", "wheel", "__init__.py"))
wheel = loader.load_module()
sys.modules["wheel"] = wheel
loader.exec_module(wheel)

def our_generic_abi():
    return [wheel.vendored.packaging.tags._normalize_string(sysconfig.get_config_var("SOABI"))]

import wheel.vendored.packaging.tags
wheel.vendored.packaging.tags._generic_abi = our_generic_abi


# --- Performance optimization: batch write support for WheelFile ---
# Adds write_batch() and writestr_batch() methods that avoid per-file overhead
# in ZipFile (seek-back to rewrite headers, _ZipWriteFile object creation, etc.)

import hashlib
import stat
from concurrent.futures import ThreadPoolExecutor
from zipfile import ZipFile, ZipInfo

import wheel.wheelfile
from wheel.wheelfile import WheelFile, get_zipinfo_datetime
from wheel.util import log, urlsafe_b64encode


def _wheelfile_write_batch(self, file_entries, max_workers=None):
    """Write multiple files to the wheel efficiently.

    file_entries is a list of (filename, arcname) tuples.

    This method reads files and computes SHA-256 hashes in parallel using
    threads, then writes them sequentially to the zip using
    ZipFile.write_batch() which avoids per-file seek-back overhead.
    """
    if not file_entries:
        return

    if max_workers is None:
        max_workers = min(8, (os.cpu_count() or 1) + 2)

    compression = self.compression
    default_algorithm = self._default_algorithm

    def _read_and_prepare(filename_arcname):
        filename, arcname = filename_arcname
        with open(filename, "rb") as f:
            st = os.fstat(f.fileno())
            data = f.read()
        zinfo = ZipInfo(
            arcname or filename, date_time=get_zipinfo_datetime(st.st_mtime)
        )
        zinfo.external_attr = (stat.S_IMODE(st.st_mode) | stat.S_IFMT(st.st_mode)) << 16
        zinfo.compress_type = compression
        # Pre-compute SHA-256 for RECORD
        hash_ = default_algorithm(data)
        hash_digest = urlsafe_b64encode(hash_.digest()).decode("ascii")
        return zinfo, data, hash_.name, hash_digest

    # Phase 1: Read files and prepare ZipInfo in parallel
    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        results = list(executor.map(_read_and_prepare, file_entries))

    # Phase 2: Write to zip using batch method (sequential, no seek-back)
    zip_entries = [(zinfo, data) for zinfo, data, _, _ in results]
    ZipFile.write_batch(self, zip_entries)

    # Phase 3: Update RECORD hashes
    record_path = self.record_path
    for (zinfo, data, hash_name, hash_digest) in results:
        fname = zinfo.filename
        log.info(f"adding '{fname}'")
        if fname != record_path:
            self._file_hashes[fname] = (hash_name, hash_digest)
            self._file_sizes[fname] = zinfo.file_size


def _wheelfile_writestr_batch(self, entries):
    """Write multiple (zinfo_or_arcname, data) pairs efficiently.

    entries is a list of (zinfo_or_arcname, data) tuples.
    """
    if not entries:
        return

    compression = self.compression

    # Normalize all entries
    prepared = []
    for zinfo_or_arcname, data in entries:
        if isinstance(zinfo_or_arcname, str):
            zinfo_or_arcname = ZipInfo(
                zinfo_or_arcname, date_time=get_zipinfo_datetime()
            )
            zinfo_or_arcname.compress_type = compression
            zinfo_or_arcname.external_attr = (0o664 | stat.S_IFREG) << 16

        if isinstance(data, str):
            data = data.encode("utf-8")

        prepared.append((zinfo_or_arcname, data))

    # Write all at once using ZipFile.write_batch
    ZipFile.write_batch(self, prepared)

    # Update RECORD hashes
    record_path = self.record_path
    default_algorithm = self._default_algorithm
    for zinfo, data in prepared:
        fname = zinfo.filename
        log.info(f"adding '{fname}'")
        if fname != record_path:
            hash_ = default_algorithm(data)
            self._file_hashes[fname] = (
                hash_.name,
                urlsafe_b64encode(hash_.digest()).decode("ascii"),
            )
            self._file_sizes[fname] = len(data)


WheelFile.write_batch = _wheelfile_write_batch
WheelFile.writestr_batch = _wheelfile_writestr_batch

