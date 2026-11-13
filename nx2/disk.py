"""Is a folder on a hard drive (a spinning disk) or on an SSD? A build reads and writes tens of gigabytes for the best part of an
hour, and an SSD makes that much faster. The answer is only ever a warning: a player may have no other drive, so the build goes
ahead on a hard drive.

spinning(path) -> True for a spinning disk, False for an SSD or flash, None when it cannot be told (a network share, an unknown
device, another operating system). Windows asks the volume for its seek penalty (IOCTL_STORAGE_QUERY_PROPERTY,
StorageDeviceSeekPenaltyProperty: a disk that seeks says so); Linux reads the kernel's queue/rotational for the device behind the
folder, through its partitions and device-mapper/RAID slaves. A Btrfs folder's device number is no disk's, so its disks are
those its mount names.
"""
import glob
import os
import stat
import sys


def existing_parent(path):
    """The nearest folder above path that exists (the path's own folder may be made later, as the game folder often is)"""
    p = os.path.abspath(os.path.expanduser(path or "."))
    while not os.path.exists(p):
        up = os.path.dirname(p)
        if up == p:
            break
        p = up
    return p


def spinning(path):
    """True on a hard drive, False on an SSD, None when the drive cannot be told"""
    folder = existing_parent(path)
    try:
        if os.name == "nt":
            return _windows_spinning(folder)
        if sys.platform.startswith("linux"):
            return _linux_spinning(folder)
    except (OSError, ValueError, AttributeError, ImportError):
        return None
    return None


def _windows_spinning(path):
    """The volume's DEVICE_SEEK_PENALTY_DESCRIPTOR (kernel32's DeviceIoControl): a spinning disk incurs a seek penalty"""
    import ctypes
    from ctypes import wintypes

    root = os.path.splitdrive(path)[0]
    if not root:
        return None
    class Query(ctypes.Structure):
        _fields_ = [("PropertyId", wintypes.DWORD), ("QueryType", wintypes.DWORD), ("Additional", wintypes.DWORD)]

    class SeekPenalty(ctypes.Structure):
        _fields_ = [("Version", wintypes.DWORD), ("Size", wintypes.DWORD), ("IncursSeekPenalty", ctypes.c_ubyte)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                     wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                         ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    kernel32.DeviceIoControl.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    invalid = wintypes.HANDLE(-1).value
    handle = kernel32.CreateFileW("\\\\.\\" + root, 0, 0x1 | 0x2, None, 3, 0, None)   # (read + write share, OPEN_EXISTING)
    if not handle or handle == invalid:
        return None
    try:
        query, answer, returned = Query(7, 0, 0), SeekPenalty(), wintypes.DWORD()   # (StorageDeviceSeekPenaltyProperty, standard query)
        ok = kernel32.DeviceIoControl(handle, 0x002D1400, ctypes.byref(query), ctypes.sizeof(query), ctypes.byref(answer),
                                      ctypes.sizeof(answer), ctypes.byref(returned), None)
        if not ok or returned.value < ctypes.sizeof(answer):
            return None
        return bool(answer.IncursSeekPenalty)
    finally:
        kernel32.CloseHandle(handle)


def _linux_spinning(path, mountinfo="/proc/self/mountinfo", sysfs="/sys"):
    """queue/rotational of the block device behind path's file system"""
    try:
        st = os.stat(path)
    except OSError:
        return None
    node = os.path.join(sysfs, "dev", "block", "%d:%d" % (os.major(st.st_dev), os.minor(st.st_dev)))
    if os.path.exists(node):
        return _device_rotational(node)
    # (Btrfs numbers its files' device itself, a number a subvolume and none of them a disk's (0:38): sysfs has no such node, and
    # the walk below ended at None, so a hard drive with Btrfs on it, as Fedora, Bazzite and CachyOS format one, never warned)
    answers = [a for a in (_device_rotational(n) for n in _mounted_from(path, mountinfo, sysfs)) if a is not None]
    return any(answers) if answers else None


def _unescaped(field):
    """A mountinfo field as the path it stands for: a space, tab, newline and backslash in it are written \\040, \\011, \\012, \\134"""
    parts = field.split("\\")
    return parts[0] + "".join(chr(int(p[:3], 8)) + p[3:] if p[:3].isdigit() else "\\" + p for p in parts[1:])


def _block_node(device, sysfs="/sys"):
    """sysfs's node of a block device file (/dev/sda2); None for anything else (a network share's or ZFS's name)"""
    try:
        st = os.stat(device)
    except OSError:
        return None
    if not stat.S_ISBLK(st.st_mode):
        return None
    return os.path.join(sysfs, "dev", "block", "%d:%d" % (os.major(st.st_rdev), os.minor(st.st_rdev)))


def _mounted_from(path, mountinfo="/proc/self/mountinfo", sysfs="/sys"):
    """sysfs's nodes of the block devices path's file system was mounted from: the one its mount names, and for a Btrfs over
    several disks every one of them (the mount names one; /sys/fs/btrfs/<id>/devices has them all)"""
    path, found = os.path.realpath(path), None
    with open(mountinfo, encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        for line in f:   # (id, parent, device, root, mount point, options... - type, source, options)
            mount, dash, source = line.rstrip("\n").partition(" - ")
            mount, source = mount.split(" "), source.split(" ")
            if not dash or len(mount) < 5 or len(source) < 2:
                continue
            point = _unescaped(mount[4])
            inside = path == point or path.startswith(point.rstrip("/") + "/")
            if inside and (found is None or len(point) >= len(found[0])):   # (the deepest; of two at one point, the later is on top)
                found = (point, source[0], _unescaped(source[1]))
    node = _block_node(found[2], sysfs) if found else None
    if not node:
        return []
    if found[1] == "btrfs":
        for devices in glob.glob(os.path.join(glob.escape(sysfs), "fs", "btrfs", "*", "devices")):
            nodes = [os.path.join(devices, n) for n in sorted(os.listdir(devices))]
            if os.path.realpath(node) in [os.path.realpath(n) for n in nodes]:
                return nodes
    return [node]


def _device_rotational(node, depth=0):
    """The device behind sysfs's node: its slaves decide for device-mapper and RAID (any spinning one makes it slow), and a
    partition's own folder has no queue/rotational, so the disk above it is read"""
    path = os.path.realpath(node)
    if depth < 4:
        slaves = os.path.join(path, "slaves")
        try:
            names = sorted(os.listdir(slaves))
        except OSError:
            names = []
        answers = [a for a in (_device_rotational(os.path.join(slaves, n), depth + 1) for n in names) if a is not None]
        if answers:
            return any(answers)
    while True:
        try:
            with open(os.path.join(path, "queue", "rotational"), encoding="ascii") as f:
                return f.read().strip() == "1"
        except OSError:
            up = os.path.dirname(path)
            if up == path:
                return None
            path = up
