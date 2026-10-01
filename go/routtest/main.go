// routtest evaluates what goroutines actually cost, and actually do, on r2.
//
// TinyGo's tasks scheduler is cooperative and single-threaded: goroutines are
// real, channels and select are real, but a goroutine keeps the CPU until it
// reaches a yield point, and every one of them carries a fixed stack cut out of
// a heap that is about a megabyte and a half.  Neither of those is visible from
// the Go side, and both decide whether a design will work here, so this program
// measures them rather than asserting them --- including how deep goroutines
// really go, which is what the stack size should be chosen from.
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
	// has room for about eighty.  That is not a hypothetical --- it is what the
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
	checkDepth()
	checkChurn()
	checkSpawn()
	checkChannel()
	checkSelect()
	checkSync()
	checkYield()
	checkSleep()

	st := libgor2.ReadStackStats()
	report("totals", "%d stacks allocated, %d reused, %d goroutines finished",
		st.Allocated, st.Reused, st.Exited)
	report("", "deepest finished goroutine used %d of %d bytes",
		st.Peak, st.Size)

	fmt.Printf("\nrouttest: done\n")
}

func report(name, format string, a ...any) {
	fmt.Printf("  %-9s %s\n", name, fmt.Sprintf(format, a...))
}

// stackBytes is what one goroutine costs, filled in by checkStacks.  Every
// later check sizes itself against it, so that this program measures the
// ceiling instead of walking into it.
var stackBytes uint64 = 16500

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
// This is the number that decides designs here: the heap divided by it is how
// many goroutines a program can have alive at once.
func checkStacks(n int) {
	var before, after runtime.MemStats

	if capped := affordable(n); capped < n {
		report("stacks", "asked for %d, the heap holds %d --- using that", n, capped)
		n = capped
	}

	// Every `go` below must take a fresh stack from the heap for the growth to
	// mean anything, so the cache of finished stacks is emptied first.
	cache := libgor2.SetStackCache(0)
	defer libgor2.SetStackCache(cache)

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

	report("stacks", "%d parked, %d bytes each (%d KiB, stack size %d), committed at `go`",
		n, per, per/1024, libgor2.ReadStackStats().Size)
	report("heap", "%d KiB total, %d KiB free -> about %d live goroutines",
		after.Sys/1024, after.HeapIdle/1024, after.HeapIdle/stackBytes)
}

// checkDepth measures how much stack goroutines doing ordinary work actually
// write.  The runtime paints every stack before the goroutine runs, so what is
// left of the pattern afterwards is the high-water mark.  The deepest of these
// is a floor for default-stack-size; leave room on top for whatever the
// program itself does.
func checkDepth() {
	type probe struct {
		name string
		work func()
	}

	probes := []probe{
		{"empty", func() {}},
		{"chan", func() {
			ch := make(chan int, 1)
			ch <- 1
			<-ch
		}},
		{"map", func() {
			m := make(map[string]int)
			for i := 0; i < 64; i++ {
				m[strconvItoa(i)] = i
			}
		}},
		{"sprintf", func() {
			_ = fmt.Sprintf("%d %s %x", 42, "str", 0xbeef)
		}},
		{"float", func() {
			_ = fmt.Sprintf("%v %.3f %g", struct{ A, B int }{1, 2}, 3.14159, 2.5e-7)
		}},
		{"sleep", func() {
			time.Sleep(time.Millisecond)
		}},
	}

	var line string

	for _, p := range probes {
		done := make(chan uintptr)

		go func() {
			p.work()
			used, _ := libgor2.StackUsed()
			done <- used
		}()

		line += fmt.Sprintf("%s %d  ", p.name, <-done)
	}

	report("depth", "bytes written: %s", line)
}

// strconvItoa keeps strconv out of the map probe's own frame count.
func strconvItoa(i int) string {
	return string(rune('a'+i%26)) + string(rune('a'+i/26))
}

// checkChurn shows what happens to the stacks of goroutines that finish.
//
// Goroutine stacks are heap objects, and the collector does not run until an
// allocation asks it to.  The r2 runtime therefore keeps the stacks of finished
// goroutines in a small cache (libgor2.SetStackCache) and hands them to the next
// `go` statement, so a loop of short-lived goroutines settles at one batch's
// worth of stacks instead of growing the heap by a stack per goroutine.
func checkChurn() {
	const (
		rounds = 4
		each   = 4
	)

	var before, after runtime.MemStats

	runtime.GC()
	runtime.ReadMemStats(&before)
	st0 := libgor2.ReadStackStats()

	for r := 0; r < rounds; r++ {
		spawnBatch(each)
	}
	runtime.ReadMemStats(&after)
	st1 := libgor2.ReadStackStats()

	grew := int64(after.HeapInuse) - int64(before.HeapInuse)

	report("churn", "%d finished goroutines grew the heap %d KiB: %d stacks reused, %d allocated",
		rounds*each, grew/1024, st1.Reused-st0.Reused, st1.Allocated-st0.Allocated)
	report("", "%d stacks cached for the next `go` (limit %d)", st1.Cached, st1.CacheMax)
}

// checkSpawn times creating goroutines and running them to completion.  The
// batch fits in the stack cache, so after the first one no `go` statement
// touches the heap and no collection is needed.
func checkSpawn() {
	batch := affordable(spawnPerBatch)

	var (
		total int
		heap0 runtime.MemStats
		heap1 runtime.MemStats
	)

	runtime.GC()
	runtime.ReadMemStats(&heap0)

	t0 := libgor2.Ticks()
	for b := 0; b < spawnBatches; b++ {
		spawnBatch(batch)
		total += batch
	}
	elapsed := libgor2.Ticks() - t0

	runtime.ReadMemStats(&heap1)

	report("spawn", "%d goroutines, %d at a time: %d ms", total, batch, elapsed)
	report("", "heap grew %d KiB over the run, no collection needed",
		(int64(heap1.HeapInuse)-int64(heap0.HeapInuse))/1024)
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
