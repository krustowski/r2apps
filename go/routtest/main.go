// routtest evaluates what goroutines actually cost, and actually do, on r2.
//
// TinyGo's tasks scheduler is cooperative and single-threaded: goroutines are
// real, channels and select are real, but a goroutine keeps the CPU until it
// reaches a yield point, and every one of them carries a fixed stack cut out of
// a heap that is about a megabyte and a half.  Neither of those is visible from
// the Go side, and both decide whether a design will work here, so this program
// measures them rather than asserting them.
//
//	routtest          run every check
//	routtest 24       park 24 goroutines for the capacity measurement
package main

import (
	"fmt"
	"runtime"
	"sync"
	"time"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
)

const (
	// How many goroutines to park at once when measuring what one costs.  The
	// ceiling is worked out from that measurement rather than by allocating
	// until something breaks.
	defaultParked = 16

	// Goroutines are created in batches, because a `go` statement allocates
	// the whole stack there and then: a loop that starts three hundred of them
	// before any of them runs needs three hundred stacks at once, and the heap
	// has room for about thirty.  That is not a hypothetical --- it is what the
	// first version of this program did, and it died with "out of memory"
	// instead of reporting a number.
	spawnBatches  = 24
	spawnPerBatch = 8

	pingCount  = 50000 // channel round trips, enough to outrun a 10 ms clock
	yieldSlots = 12    // how much of the interleaving log to show
)

func main() {
	parked := defaultParked
	if args := libgor2.Args(); len(args) > 1 {
		if n := atoi(args[1]); n > 0 {
			parked = n
		}
	}

	fmt.Printf("routtest: goroutines on r2\n\n")

	checkBasic()
	checkStacks(parked)
	checkChurn()
	checkSpawn()
	checkChannel()
	checkSelect()
	checkSync()
	checkYield()
	checkSleep()

	fmt.Printf("\nrouttest: done\n")
}

func report(name, format string, a ...any) {
	fmt.Printf("  %-9s %s\n", name, fmt.Sprintf(format, a...))
}

// stackBytes is what one goroutine costs, filled in by checkStacks.  Every
// later check sizes itself against it, so that this program measures the
// ceiling instead of walking into it.
var stackBytes uint64 = 33000

// affordable caps want at the number of goroutines the free heap can hold,
// keeping a quarter of it back as margin.
func affordable(want int) int {
	var m runtime.MemStats
	runtime.ReadMemStats(&m)

	most := int(m.HeapIdle * 3 / 4 / stackBytes)
	if most < 1 {
		most = 1
	}

	if want > most {
		return most
	}

	return want
}

// checkBasic is the one that has to pass before any of the numbers below mean
// anything: a goroutine runs, and a value comes back over a channel.
func checkBasic() {
	ch := make(chan int)

	go func() {
		ch <- 42
	}()

	if v := <-ch; v != 42 {
		report("basic", "FAIL: got %d, want 42", v)

		return
	}

	report("basic", "ok: goroutine ran, channel delivered")
}

// checkStacks measures what a goroutine costs by parking a known number of them
// and watching the heap, then works out how many could ever be alive at once.
//
// This is the number that decides designs here.  A goroutine per connection is
// an ordinary thing to write in Go and is not affordable on r2.
func checkStacks(n int) {
	var before, after runtime.MemStats

	if capped := affordable(n); capped < n {
		report("stacks", "asked for %d, the heap holds %d --- using that", n, capped)
		n = capped
	}

	runtime.GC()
	runtime.ReadMemStats(&before)

	release := make(chan struct{})
	var wg sync.WaitGroup

	wg.Add(n)
	for i := 0; i < n; i++ {
		go func() {
			defer wg.Done()
			<-release
		}()
	}

	// Every one of them is now blocked on the receive, so their stacks are all
	// live at the same time.
	runtime.Gosched()
	runtime.ReadMemStats(&after)

	close(release)
	wg.Wait()

	if after.HeapInuse <= before.HeapInuse {
		report("stacks", "FAIL: heap did not grow while %d goroutines were parked", n)

		return
	}

	per := (after.HeapInuse - before.HeapInuse) / uint64(n)
	if per > 0 {
		stackBytes = per
	}

	report("stacks", "%d parked, %d bytes each (%d KiB), committed at `go`",
		n, per, per/1024)
	report("heap", "%d KiB total, %d KiB free -> about %d live goroutines",
		after.Sys/1024, after.HeapIdle/1024, after.HeapIdle/stackBytes)
}

// checkChurn is the finding that decides how a long-running Go service has to
// be written here.
//
// Goroutine stacks are ordinary heap objects, and the collector does not run
// until an allocation asks it to.  A loop that starts short-lived goroutines
// therefore grows the heap steadily even though every one of them has already
// finished --- and because a stack is 32 KiB of *contiguous* heap, it runs out
// of room sooner than the free byte count suggests.
func checkChurn() {
	const (
		rounds = 4
		each   = 4
	)

	var before, after, collected runtime.MemStats

	runtime.GC()
	runtime.ReadMemStats(&before)

	for r := 0; r < rounds; r++ {
		spawnBatch(each)
	}
	runtime.ReadMemStats(&after)

	runtime.GC()
	runtime.ReadMemStats(&collected)

	report("churn", "%d finished goroutines grew the heap %d KiB, %d frees",
		rounds*each, (after.HeapInuse-before.HeapInuse)/1024, after.Frees-before.Frees)
	report("", "runtime.GC() returned %d KiB --- nothing else will",
		(after.HeapInuse-collected.HeapInuse)/1024)
}

// checkSpawn times creating goroutines and running them to completion.
func checkSpawn() {
	batch := affordable(spawnPerBatch)

	var (
		total         int
		spawnMS, gcMS uint64
	)

	for b := 0; b < spawnBatches; b++ {
		t0 := libgor2.Ticks()
		spawnBatch(batch)

		// Not tidiness: see checkChurn.  Without this the heap only grows, and
		// a long enough loop stops with "out of memory".
		t1 := libgor2.Ticks()
		runtime.GC()
		t2 := libgor2.Ticks()

		spawnMS += t1 - t0
		gcMS += t2 - t1
		total += batch
	}

	report("spawn", "%d goroutines, %d at a time: %d ms running, %d ms collecting",
		total, batch, spawnMS, gcMS)
	report("", "%d collections of a %d KiB heap dominate the cost",
		spawnBatches, heapKiB())
}

func spawnBatch(n int) {
	var wg sync.WaitGroup

	wg.Add(n)
	for i := 0; i < n; i++ {
		go wg.Done()
	}
	wg.Wait()
}

// checkChannel times a round trip: one goroutine receives and sends back, which
// is two scheduler switches per iteration.  Nothing is allocated in the loop,
// so this is the cost of the switch itself.
func checkChannel() {
	var (
		req  = make(chan int)
		resp = make(chan int)
	)

	go func() {
		for v := range req {
			resp <- v + 1
		}
		close(resp)
	}()

	start := libgor2.Ticks()

	for i := 0; i < pingCount; i++ {
		req <- i

		if v := <-resp; v != i+1 {
			report("chan", "FAIL: got %d, want %d", v, i+1)

			return
		}
	}

	elapsed := libgor2.Ticks() - start
	close(req)

	report("chan", "%d round trips in %d ms, %s per trip",
		pingCount, elapsed, perOp(elapsed, pingCount))
}

// checkSelect covers select over several channels, and the default arm.
func checkSelect() {
	var (
		a = make(chan string, 1)
		b = make(chan string, 1)
	)

	select {
	case <-a:
		report("select", "FAIL: read from an empty channel")

		return
	default:
	}

	go func() { b <- "b" }()

	var got string

	select {
	case got = <-a:
	case got = <-b:
	}

	if got != "b" {
		report("select", "FAIL: got %q, want \"b\"", got)

		return
	}

	report("select", "ok: default arm and multi-channel receive")
}

// checkSync covers WaitGroup and Mutex.  Neither has anything to protect
// against on a single-threaded scheduler, but code meant to compile against
// real Go uses them, and they have to behave.
func checkSync() {
	const (
		workers = 8
		each    = 500
	)

	var (
		mu    sync.Mutex
		wg    sync.WaitGroup
		total int
	)

	wg.Add(workers)
	for i := 0; i < workers; i++ {
		go func() {
			defer wg.Done()

			for j := 0; j < each; j++ {
				mu.Lock()
				total++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()

	if want := workers * each; total != want {
		report("sync", "FAIL: counter is %d, want %d", total, want)

		return
	}

	report("sync", "ok: %d workers x %d increments under a mutex", workers, each)
}

// checkYield is the one that catches people out.
//
// The scheduler is cooperative: a goroutine that never blocks never gives the
// CPU up, so three of them run strictly one after another.  Put a Gosched in
// the loop and they interleave.  The logs below show which happened.
func checkYield() {
	report("yield", "without Gosched: %q", interleaving(false))
	report("", "with Gosched:    %q", interleaving(true))
}

func interleaving(yield bool) string {
	const (
		runners = 3
		rounds  = 8
	)

	var (
		wg  sync.WaitGroup
		log = make([]byte, 0, runners*rounds)
	)

	wg.Add(runners)
	for i := 0; i < runners; i++ {
		id := byte('a' + i)

		go func() {
			defer wg.Done()

			for r := 0; r < rounds; r++ {
				sink += id // a little work, so this is not optimised away

				// Appending to a shared slice from several goroutines is only
				// safe because this scheduler is cooperative and neither the
				// append nor the work above yields.  Under real Go this is a
				// data race.
				log = append(log, id)

				if yield {
					runtime.Gosched()
				}
			}
		}()
	}
	wg.Wait()

	if len(log) > yieldSlots {
		log = log[:yieldSlots]
	}

	return string(log)
}

var sink byte

// checkSleep confirms that a sleeping goroutine yields to a runnable one, and
// that the sleep is honoured.  The clock ticks every 10 ms, so a 50 ms sleep is
// allowed to come back anywhere from 50 to 60.
func checkSleep() {
	const nap = 50 * time.Millisecond

	var (
		wg      sync.WaitGroup
		counter int
		stop    bool
	)

	wg.Add(1)
	go func() {
		defer wg.Done()

		for !stop {
			counter++
			runtime.Gosched()
		}
	}()

	start := libgor2.Ticks()
	time.Sleep(nap)
	elapsed := libgor2.Ticks() - start

	stop = true
	wg.Wait()

	if counter == 0 {
		report("sleep", "FAIL: nothing else ran while main slept")

		return
	}

	report("sleep", "slept %d ms (asked %d), other goroutine ran %d times",
		elapsed, nap/time.Millisecond, counter)
}

// perOp turns a total in milliseconds into a per-operation figure.  The clock
// only moves every 10 ms, so a run too short to measure says so rather than
// reporting a confident zero.
func heapKiB() uint64 {
	var m runtime.MemStats
	runtime.ReadMemStats(&m)

	return m.Sys / 1024
}

func perOp(ms uint64, n int) string {
	if ms < 20 {
		return "too little to measure against a 10 ms clock"
	}

	if us := ms * 1000 / uint64(n); us > 0 {
		return fmt.Sprintf("%d us", us)
	}

	return fmt.Sprintf("%d ns", ms*1000000/uint64(n))
}

func atoi(s string) int {
	n := 0
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0
		}

		n = n*10 + int(s[i]-'0')
	}

	return n
}
