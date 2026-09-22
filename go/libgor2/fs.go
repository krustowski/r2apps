package libgor2

import "unsafe"

// Buffer sizes fixed by the ABI.  The kernel is told none of them, so a buffer
// shorter than this is simply overrun.
const (
	maxDirEntries    = 32 // syscall 0x28
	maxVfsDirEntries = 64 // syscall 0x2d
	maxMounts        = 8  // syscall 0x2c
)

// readChunk is how much of a file ReadFile asks for at a time.  The whole file
// still has to fit in the heap, which is about 1.5 MiB.
const readChunk = 4096

// ReadFile reads the whole of name and returns its contents.
//
// It reads in chunks through syscall 0x39, which is told how much room the
// buffer has, rather than through syscall 0x20, which is not.
func ReadFile(name string) ([]byte, error) {
	var (
		out []byte
		buf = make([]byte, readChunk)
		off uint64
	)

	for {
		n, e := ReadFileAt(name, buf, off)
		if e != nil {
			return nil, e
		}

		if n == 0 {
			break
		}

		out = append(out, buf[:n]...)
		off += uint64(n)

		// A short read means the end of the file.
		if n < len(buf) {
			break
		}
	}

	return out, nil
}

// ReadFileAt reads at most len(buf) bytes of name starting offset bytes in, and
// returns how many it got: short at the end of the file, zero when offset is
// past it (syscall 0x39).  Works on the floppy and on the CD alike.
func ReadFileAt(name string, buf []byte, offset uint64) (int, error) {
	if len(buf) == 0 {
		return 0, nil
	}

	nameBuf := cstring(name)
	req := fileRange{
		buffer: uint64(ptr(unsafe.Pointer(&buf[0]))),
		offset: offset,
		length: uint64(len(buf)),
	}

	ret := int64(Syscall(ScReadFileAt,
		ptr(unsafe.Pointer(&nameBuf[0])),
		ptr(unsafe.Pointer(&req))))
	if ret < 0 {
		return 0, EFilesystemError
	}

	return int(ret), nil
}

// ReadFileInto reads the whole of name into buf using syscall 0x20.
//
// The kernel is never told how big buf is and will write the entire file
// wherever the pointer leads, so buf has to be big enough for the largest file
// this will ever be handed.  ReadFile is the one to reach for; this is here
// because the syscall is part of the ABI.
func ReadFileInto(name string, buf []byte) error {
	if len(buf) == 0 {
		return EInvalidInput
	}

	nameBuf := cstring(name)

	return err(Syscall(ScReadFile,
		ptr(unsafe.Pointer(&nameBuf[0])),
		ptr(unsafe.Pointer(&buf[0]))))
}

// WriteFileAt writes data into name starting offset bytes in, creating the file
// if it is missing and growing it if it is too short (syscall 0x3a).  What lies
// before offset is left alone, so a file can be added to rather than replaced.
//
// FAT12 only: the ISO is read-only.
func WriteFileAt(name string, data []byte, offset uint64) (int, error) {
	if len(data) == 0 {
		return 0, nil
	}

	nameBuf := cstring(name)
	req := fileRange{
		buffer: uint64(ptr(unsafe.Pointer(&data[0]))),
		offset: offset,
		length: uint64(len(data)),
	}

	ret := int64(Syscall(ScWriteFileAt,
		ptr(unsafe.Pointer(&nameBuf[0])),
		ptr(unsafe.Pointer(&req))))
	if ret < 0 {
		return 0, EFilesystemError
	}

	return int(ret), nil
}

// WriteFile replaces name with a fixed 512-byte block (syscall 0x21).  Anything
// shorter is padded; anything longer is truncated.  WriteFileAt is the way to
// write a file of any other size.
func WriteFile(name string, data []byte) error {
	buf := make([]byte, 512)
	copy(buf, data)

	nameBuf := cstring(name)

	return err(Syscall(ScWriteFile,
		ptr(unsafe.Pointer(&nameBuf[0])),
		ptr(unsafe.Pointer(&buf[0]))))
}

// Rename renames a directory entry (syscall 0x22).
func Rename(oldName, newName string) error {
	o, n := cstring(oldName), cstring(newName)

	return err(Syscall(ScRenameFile,
		ptr(unsafe.Pointer(&o[0])),
		ptr(unsafe.Pointer(&n[0]))))
}

// Delete removes a directory entry (syscall 0x23).
func Delete(name string) error {
	b := cstring(name)

	return err(Syscall(ScDeleteFile, ptr(unsafe.Pointer(&b[0])), 0))
}

// Mkdir creates a subdirectory called name inside parent, an absolute VFS path
// (syscall 0x27).
func Mkdir(parent, name string) error {
	p, n := cstring(parent), cstring(name)

	return err(Syscall(ScWriteSubdir,
		ptr(unsafe.Pointer(&p[0])),
		ptr(unsafe.Pointer(&n[0]))))
}

// Chdir changes the working directory to an absolute VFS path (syscall 0x2e).
//
// The working directory belongs to the kernel, not to the process, so this
// changes it for the shell and every other task too.
func Chdir(path string) error {
	b := cstring(path)

	return err(Syscall(ScChdir, ptr(unsafe.Pointer(&b[0])), 0))
}

// ListDir lists a FAT12 directory by cluster number (syscall 0x28).
func ListDir(cluster uint64) ([]Entry, error) {
	var buf [maxDirEntries]Entry

	if e := err(Syscall(ScListDir, uintptr(cluster), ptr(unsafe.Pointer(&buf[0])))); e != nil {
		return nil, e
	}

	// The syscall reports no count, so an entry whose first name byte is zero
	// marks the end of the used part of the table, as it does on disk.
	n := 0
	for n < len(buf) && buf[n].Name[0] != 0 {
		n++
	}

	out := make([]Entry, n)
	copy(out, buf[:n])

	return out, nil
}

// ListDirPath lists the directory at an absolute VFS path, on either filesystem
// (syscall 0x2d).
func ListDirPath(path string) ([]VfsDirEntry, error) {
	var buf [maxVfsDirEntries]VfsDirEntry

	b := cstring(path)

	ret := int64(Syscall(ScListDirPath,
		ptr(unsafe.Pointer(&b[0])),
		ptr(unsafe.Pointer(&buf[0]))))
	if ret < 0 || ret > int64(len(buf)) {
		return nil, EFilesystemError
	}

	out := make([]VfsDirEntry, ret)
	copy(out, buf[:ret])

	return out, nil
}

// ListMounts returns the VFS mount table (syscall 0x2c).
func ListMounts() []MountInfo {
	var buf [maxMounts]MountInfo

	n := int(Syscall(ScListMounts, 0, ptr(unsafe.Pointer(&buf[0]))))
	if n <= 0 || n > len(buf) {
		return nil
	}

	out := make([]MountInfo, n)
	copy(out, buf[:n])

	return out
}

// Fsck runs a filesystem check and returns its report (syscall 0x2b).
func Fsck() (*FsckReport, error) {
	report := &FsckReport{}

	if e := err(Syscall(ScRunFsCheck, 0, ptr(unsafe.Pointer(report)))); e != nil {
		return nil, e
	}

	return report, nil
}
