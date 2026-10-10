//go:build r2abimock && !r2

package libgor2

import (
	"testing"
	"unsafe"
)

func TestNetworkMonitoringABI(t *testing.T) {
	if unsafe.Sizeof(NetStats{}) != 40 || unsafe.Offsetof(NetStats{}.TXBytes) != 32 ||
		unsafe.Sizeof(NetPortBinding{}) != 32 || unsafe.Offsetof(NetPortBinding{}.PID) != 8 ||
		unsafe.Offsetof(NetPortBinding{}.Name) != 16 || unsafe.Sizeof(NetPortTable{}) != 528 ||
		unsafe.Offsetof(NetPortTable{}.NPorts) != 8 || unsafe.Offsetof(NetPortTable{}.Bindings) != 16 {
		t.Fatal("network monitoring layout differs from kernel ABI")
	}
	stats := NetStats{RXBytes: 99}
	table := NetPortTable{DriverPID: NetNoPID}
	expected := NetStats{TimestampMS: 1500, RXFrames: 2, RXBytes: 120, TXFrames: 1, TXBytes: 60}
	mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
		if arg2 != 0 || arg3 != 0 {
			t.Fatal("unexpected monitoring arguments")
		}
		switch number {
		case ScNetStats:
			if arg1 != uintptr(unsafe.Pointer(&stats)) {
				t.Fatal("wrong stats output")
			}
			*(*NetStats)(unsafe.Pointer(arg1)) = expected
		case ScNetPorts:
			if arg1 != uintptr(unsafe.Pointer(&table)) {
				t.Fatal("wrong table output")
			}
			table.NPorts = 1
			table.Bindings[0] = NetPortBinding{Port: 80, PID: 1007, Name: [16]byte{'g', 'a', 'r', 'n'}}
		default:
			t.Fatal("wrong monitoring syscall")
		}
		return 0
	})
	if e := ReadNetStats(&stats); e != nil || stats != expected {
		t.Fatalf("stats: %+v, %v", stats, e)
	}
	if e := ReadNetPorts(&table); e != nil || table.Bindings[0].PID != 1007 {
		t.Fatalf("table: %+v, %v", table, e)
	}
}

func TestNetworkMonitoringErrorsPreserveOutput(t *testing.T) {
	for _, code := range []Errno{EBusy, EInvalidInput, EInvalidSyscall} {
		stats := NetStats{TimestampMS: 1, RXBytes: 2}
		table := NetPortTable{DriverPID: 999, NPorts: 1}
		beforeStats, beforeTable := stats, table
		mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr { return uintptr(code) })
		if e := ReadNetStats(&stats); e != code || stats != beforeStats {
			t.Fatal("stats error/output")
		}
		if e := ReadNetPorts(&table); e != code || table != beforeTable {
			t.Fatal("ports error/output")
		}
		if _, found, e := NetPortOwner(80); found || e != code {
			t.Fatal("owner error lost")
		}
	}
}

func TestNetworkMonitoringRejectsNil(t *testing.T) {
	mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr { t.Fatal("invalid argument reached kernel"); return 0 })
	if ReadNetStats(nil) != EInvalidInput || ReadNetPorts(nil) != EInvalidInput {
		t.Fatal("nil output accepted")
	}
	if _, found, e := NetPortOwner(0); found || e != EInvalidInput {
		t.Fatal("zero port accepted")
	}
}

func TestTCPPortOwnerLookup(t *testing.T) {
	count := uint8(16)
	expected := NetPortBinding{Port: 65535, PID: 100000, Name: [16]byte{'t', 'n', 't'}}
	mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
		if number != ScNetPorts || arg2 != 0 || arg3 != 0 {
			t.Fatal("wrong owner query")
		}
		table := (*NetPortTable)(unsafe.Pointer(arg1))
		*table = NetPortTable{DriverPID: NetNoPID, NPorts: count}
		table.Bindings[15] = expected
		return 0
	})
	if owner, found, e := NetPortOwner(65535); e != nil || !found || owner != expected {
		t.Fatalf("owner: %+v, %v, %v", owner, found, e)
	}
	if _, found, e := NetPortOwner(81); e != nil || found {
		t.Fatal("unbound port found")
	}
	count = 0
	if _, found, e := NetPortOwner(65535); e != nil || found {
		t.Fatal("unused tail entry consulted")
	}
	count = 17
	if _, found, e := NetPortOwner(65535); e != EInvalidInput || found {
		t.Fatal("invalid table count accepted")
	}
}
