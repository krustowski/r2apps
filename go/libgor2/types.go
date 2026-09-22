package libgor2

import "unsafe"

// The kernel reads and writes these structures directly in our memory, so each
// one has to match the packed C layout in c/libcr2/syscall.h byte for byte.
//
// Go has no `packed` attribute: it aligns every field to its own width.  Three
// of the ABI structures put a wide field at an odd offset, and for those the
// field is held as bytes and read through an accessor.  The size assertions at
// the bottom of this file are what keeps all of this honest --- get a layout
// wrong and the package stops compiling rather than quietly handing the kernel
// a pointer to the wrong shape.

// SysInfo is the system information block (syscall 0x01).
type SysInfo struct {
	Name        [32]byte
	User        [32]byte
	Path        [32]byte
	Version     [8]byte
	PathCluster uint32
	Uptime      uint32 // seconds since boot
	IP          [4]byte
}

// RTC is a real-time-clock reading (syscall 0x02).
//
// Year sits at offset 5, so it cannot be a uint16 field: Go would align it to
// 6 and shift the structure out from under the kernel.
type RTC struct {
	Seconds uint8
	Minutes uint8
	Hours   uint8
	Day     uint8
	Month   uint8
	yearLo  uint8
	yearHi  uint8
}

// Year is the four-digit year.
func (r *RTC) Year() uint16 {
	return uint16(r.yearLo) | uint16(r.yearHi)<<8
}

// Entry is a FAT12 directory entry (syscall 0x28).
type Entry struct {
	Name           [8]byte
	Ext            [3]byte
	Attr           uint8
	Reserved       uint8
	Tenths         uint8
	CreateTime     uint16
	CreateDate     uint16
	LastAccessTime uint16
	HighCluster    uint16
	WriteTime      uint16
	WriteDate      uint16
	StartCluster   uint16
	FileSize       uint32
}

// IsDir reports whether the entry is a subdirectory.
func (e *Entry) IsDir() bool {
	return e.Attr&0x10 != 0
}

// FsckReport is the result of a filesystem check (syscall 0x2b).
type FsckReport struct {
	Errors         uint64
	OrphanClusters uint64
	CrossLinked    uint64
	InvalidEntries uint64
}

// Filesystem kinds reported by MountInfo.FsType.
const (
	FsNone    = 0
	FsRootfs  = 1
	FsFat12   = 2
	FsIso9660 = 3
)

// MountInfo describes one VFS mount point (syscall 0x2c).
type MountInfo struct {
	rawPath [32]byte
	PathLen uint8
	FsType  uint8
}

// Path is the mount point, trimmed to its actual length.
func (m *MountInfo) Path() string {
	n := int(m.PathLen)
	if n > len(m.rawPath) {
		n = len(m.rawPath)
	}

	return string(m.rawPath[:n])
}

// VfsDirEntry is one member of a directory, on either filesystem (syscall 0x2d).
//
// Size is four bytes at offset 34, which is not where Go would put a uint32.
type VfsDirEntry struct {
	rawName [32]byte
	NameLen uint8
	isDir   uint8
	rawSize [4]byte
}

// Name is the entry name, trimmed to its actual length.
func (v *VfsDirEntry) Name() string {
	n := int(v.NameLen)
	if n > len(v.rawName) {
		n = len(v.rawName)
	}

	return string(v.rawName[:n])
}

// IsDir reports whether this entry is a directory.
func (v *VfsDirEntry) IsDir() bool {
	return v.isDir != 0
}

// Size is the file size in bytes.
func (v *VfsDirEntry) Size() uint32 {
	return uint32(v.rawSize[0]) | uint32(v.rawSize[1])<<8 |
		uint32(v.rawSize[2])<<16 | uint32(v.rawSize[3])<<24
}

// FBInfo describes the VESA linear framebuffer (syscall 0x16).
type FBInfo struct {
	Width  uint32
	Height uint32
	Pitch  uint32
	BPP    uint32
}

// NetStatus is the network status block (syscall 0x38).
type NetStatus struct {
	MAC       [6]byte
	IP        [4]byte
	DrvActive uint8
	NPorts    uint8
	Ports     [16]uint16
}

// Task modes and statuses, as reported by TaskInfo.
const (
	ModeKernel = 0
	ModeUser   = 1

	StatusReady   = 0
	StatusRunning = 1
	StatusIdle    = 2
	StatusBlocked = 3
	StatusCrashed = 4
	StatusDead    = 5
)

// TaskInfo describes one task (syscall 0x2f).
//
// RIP is eight bytes at offset 20; Go would align a uint64 to 24.
type TaskInfo struct {
	ID      uint8
	Mode    uint8
	Status  uint8
	pad     uint8
	rawName [16]byte
	rawRIP  [8]byte
}

// Name is the task name with its padding removed.
func (t *TaskInfo) Name() string {
	n := len(t.rawName)
	for n > 0 && (t.rawName[n-1] == ' ' || t.rawName[n-1] == 0) {
		n--
	}

	return string(t.rawName[:n])
}

// RIP is where the task was when it was last put down, or 0 for the task doing
// the asking.  A task that has stopped answering reports the same RIP every
// time, and that address says which code it is stuck in.
func (t *TaskInfo) RIP() uint64 {
	var v uint64
	for i := 7; i >= 0; i-- {
		v = v<<8 | uint64(t.rawRIP[i])
	}

	return v
}

// MousePacket is one PS/2 mouse event (syscall 0x03, subfunction 0x05).
// Dy is positive when the mouse moves up, following the PS/2 convention.
type MousePacket struct {
	Buttons uint8
	Dx      int8
	Dy      int8
}

// Mouse button bits.
const (
	MouseLeft   = 1 << 0
	MouseRight  = 1 << 1
	MouseMiddle = 1 << 2
)

// fileRange is the argument block for the ranged read and write syscalls
// (0x39, 0x3a).
type fileRange struct {
	buffer uint64
	offset uint64
	length uint64
}

// Layout assertions.  Each pair fails to compile if the Go structure is larger
// or smaller than the packed C structure the kernel expects.
const (
	_ = uint(unsafe.Sizeof(SysInfo{}) - 116)
	_ = uint(116 - unsafe.Sizeof(SysInfo{}))

	_ = uint(unsafe.Sizeof(RTC{}) - 7)
	_ = uint(7 - unsafe.Sizeof(RTC{}))

	_ = uint(unsafe.Sizeof(Entry{}) - 32)
	_ = uint(32 - unsafe.Sizeof(Entry{}))

	_ = uint(unsafe.Sizeof(FsckReport{}) - 32)
	_ = uint(32 - unsafe.Sizeof(FsckReport{}))

	_ = uint(unsafe.Sizeof(MountInfo{}) - 34)
	_ = uint(34 - unsafe.Sizeof(MountInfo{}))

	_ = uint(unsafe.Sizeof(VfsDirEntry{}) - 38)
	_ = uint(38 - unsafe.Sizeof(VfsDirEntry{}))

	_ = uint(unsafe.Sizeof(FBInfo{}) - 16)
	_ = uint(16 - unsafe.Sizeof(FBInfo{}))

	_ = uint(unsafe.Sizeof(NetStatus{}) - 44)
	_ = uint(44 - unsafe.Sizeof(NetStatus{}))

	_ = uint(unsafe.Sizeof(TaskInfo{}) - 28)
	_ = uint(28 - unsafe.Sizeof(TaskInfo{}))

	_ = uint(unsafe.Sizeof(MousePacket{}) - 3)
	_ = uint(3 - unsafe.Sizeof(MousePacket{}))

	_ = uint(unsafe.Sizeof(fileRange{}) - 24)
	_ = uint(24 - unsafe.Sizeof(fileRange{}))
)
