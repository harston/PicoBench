// armstat runs a ROM headless in Gopher2600 and reports how much work the
// cartridge's ARM coprocessor does per frame: Thumb instructions, ARM cycles
// in Gopher2600's Harmony (LPC2103 @ 70 MHz, MAM) timing model, data accesses
// by memory region and the Thumb instruction mix. With an ARM clock argument
// below 70 it also shows whether the game still keeps a stable frame, i.e.
// the slowest ARM the game tolerates.
//
// Needs the Stat* counters from gopher2600_instrumentation.patch.
//
// usage: armstat <rom> <frames> [play [armMHz]]
//
//	play    scripted input: RESET at frame 120, FIRE at 240 and 420, then a
//	        random joystick direction and fire state every 20 frames
//	        (fixed seed, so runs are repeatable)
//	armMHz  Harmony ARM clock for the timing model (default 70)
//
// Recording of ARM program runs (arm/armrec.go, format in
// tools/arm_carts/README.md) is switched on by environment variables:
//
//	ARMREC=file        write the recording here
//	ARMREC_FROM=60     first frame in which calls may be recorded
//	ARMREC_FIRST=50    record this many consecutive calls from ARMREC_FROM on
//	ARMREC_EVERY=0     after those, also every n-th call (0 = none)
//	ARMREC_MAX=200     never more than this many calls in total
package main

import (
	"fmt"
	"os"
	"sort"
	"strconv"

	"github.com/jetsetilly/gopher2600/cartridgeloader"
	"github.com/jetsetilly/gopher2600/debugger/govern"
	"github.com/jetsetilly/gopher2600/environment"
	"github.com/jetsetilly/gopher2600/hardware"
	"github.com/jetsetilly/gopher2600/hardware/memory/cartridge/arm"
	"github.com/jetsetilly/gopher2600/hardware/riot/ports"
	"github.com/jetsetilly/gopher2600/hardware/riot/ports/plugging"
	"github.com/jetsetilly/gopher2600/hardware/television"
	"github.com/jetsetilly/gopher2600/hardware/television/coords"
	"github.com/jetsetilly/gopher2600/setup"
	"math/rand"
)

func envInt(name string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil {
		return v
	}
	return def
}

func main() {
	file := os.Args[1]
	frames, _ := strconv.Atoi(os.Args[2])
	play := len(os.Args) > 3 && os.Args[3] == "play"
	rng := rand.New(rand.NewSource(1))
	tv, err := television.NewTelevision("AUTO")
	if err != nil {
		panic(err)
	}
	defer tv.End()
	tv.SetFPSLimit(false)
	vcs, err := hardware.NewVCS(environment.MainEmulation, tv, nil, nil)
	if err != nil {
		panic(err)
	}
	vcs.Env.Normalise()
	if len(os.Args) > 4 {
		mhz, _ := strconv.ParseFloat(os.Args[4], 64)
		vcs.Env.Prefs.Cartridge.ARM.Clock.Set(mhz)
	}
	cl, err := cartridgeloader.NewLoaderFromFilename(file, "AUTO", "AUTO", nil, tv)
	if err != nil {
		panic(err)
	}
	defer cl.Close()
	if err := setup.AttachCartridge(vcs, cl, nil); err != nil {
		panic(err)
	}
	if path := os.Getenv("ARMREC"); path != "" {
		f, err := os.Create(path)
		if err != nil {
			panic(err)
		}
		defer f.Close()
		from, first := envInt("ARMREC_FROM", 60), envInt("ARMREC_FIRST", 50)
		every, max := envInt("ARMREC_EVERY", 0), envInt("ARMREC_MAX", 200)
		firstIndex := -1
		arm.RecFile = f
		arm.RecSelect = func(index int, frame int) bool {
			if frame < from || arm.RecWritten >= max {
				return false
			}
			if firstIndex < 0 {
				firstIndex = index
			}
			if index-firstIndex < first {
				return true
			}
			return every > 0 && index%every == 0
		}
	}
	type fs struct {
		ins, runs           uint64
		cyc                 float64
		romR, ramR, ramW, o uint64
		lines               int
	}
	per := []fs{}
	var last fs
	lastFrame := -1
	var lastCoords coords.TelevisionCoords
	snap := func() fs {
		return fs{arm.StatInstructions, arm.StatRuns, arm.StatCycles, arm.StatRomReads, arm.StatRamReads, arm.StatRamWrites, arm.StatOtherAcc, 0}
	}
	err = vcs.RunForFrameCount(frames, func() (govern.State, error) {
		c := vcs.TV.GetCoords()
		if c.Frame != lastFrame {
			if lastFrame >= 0 {
				n := snap()
				d := fs{n.ins - last.ins, n.runs - last.runs, n.cyc - last.cyc, n.romR - last.romR, n.ramR - last.ramR, n.ramW - last.ramW, n.o - last.o, lastCoords.Scanline}
				per = append(per, d)
				last = n
			} else {
				last = snap()
			}
			lastFrame = c.Frame
			arm.StatFrame = c.Frame
			arm.RecFrame = c.Frame
			if play {
				f := c.Frame
				ev := func(port plugging.PortID, e ports.Event, d ports.EventData) {
					vcs.RIOT.Ports.HandleInputEvent(ports.InputEvent{Port: port, Ev: e, D: d})
				}
				switch {
				case f == 120:
					ev(plugging.PortPanel, ports.PanelReset, true)
				case f == 126:
					ev(plugging.PortPanel, ports.PanelReset, false)
				case f == 240 || f == 420:
					ev(plugging.PortLeft, ports.Fire, true)
				case f == 246 || f == 426:
					ev(plugging.PortLeft, ports.Fire, false)
				case f > 480 && f%20 == 0:
					dirs := []ports.Event{ports.Left, ports.Right, ports.Up, ports.Down, ports.LeftUp, ports.RightUp, ports.Centre}
					d := dirs[rng.Intn(len(dirs))]
					if d == ports.Centre {
						ev(plugging.PortLeft, d, nil)
					} else {
						ev(plugging.PortLeft, d, ports.DataStickSet)
					}
					ev(plugging.PortLeft, ports.Fire, rng.Intn(2) == 0)
				}
			}
		}
		lastCoords = c
		if vcs.CPU.Jammed {
			return govern.Ending, fmt.Errorf("CPU JAM")
		}
		return govern.Running, nil
	})
	if err != nil {
		fmt.Println("ERR", err)
	}
	// skip first 60 frames (boot)
	skip := 60
	if len(per) <= skip {
		skip = 0
	}
	p := per[skip:]
	var tot fs
	cycs := make([]float64, 0, len(p))
	inss := make([]float64, 0, len(p))
	for _, f := range p {
		tot.ins += f.ins
		tot.runs += f.runs
		tot.cyc += f.cyc
		tot.romR += f.romR
		tot.ramR += f.ramR
		tot.ramW += f.ramW
		tot.o += f.o
		cycs = append(cycs, f.cyc)
		inss = append(inss, float64(f.ins))
	}
	sort.Float64s(cycs)
	sort.Float64s(inss)
	n := float64(len(p))
	pct := func(a []float64, q float64) float64 { return a[int(q*float64(len(a)-1))] }
	bad := 0
	lc := map[int]int{}
	for _, f := range p {
		lc[f.lines]++
	}
	mode, modeN := 0, 0
	for k, v := range lc {
		if v > modeN {
			mode, modeN = k, v
		}
	}
	for _, f := range p {
		if f.lines != mode {
			bad++
		}
	}
	fmt.Printf("%s\n", cl.Name)
	clk := vcs.Env.Prefs.Cartridge.ARM.Clock.Get().(float64)
	fmt.Printf("  effMIPS=%.1f\n", float64(tot.ins)/(tot.cyc/clk))
	fmt.Printf("  armclk=%v lines/frame mode=%d offframes=%d/%d maxRunInstr=%d\n", vcs.Env.Prefs.Cartridge.ARM.Clock.Get(), mode+1, bad, len(p), arm.StatMaxRun)
	fmt.Printf("  mapper=%s tv=%s frames=%d (measured %d)\n", vcs.Mem.Cart.ID(), vcs.TV.GetFrameInfo().Spec.ID, len(per), len(p))
	fmt.Printf("  ARM runs/frame=%.2f  instr/frame avg=%.0f p50=%.0f p95=%.0f max=%.0f\n", float64(tot.runs)/n, float64(tot.ins)/n, pct(inss, .5), pct(inss, .95), inss[len(inss)-1])
	fmt.Printf("  ARM cycles/frame (70MHz model) avg=%.0f p95=%.0f max=%.0f  => max %.2f ms/frame, CPI=%.2f\n", tot.cyc/n, pct(cycs, .95), cycs[len(cycs)-1], cycs[len(cycs)-1]/(1000.0*vcs.Env.Prefs.Cartridge.ARM.Clock.Get().(float64)), tot.cyc/float64(tot.ins))
	fmt.Printf("  data acc/instr: romR=%.3f ramR=%.3f ramW=%.3f other=%.4f\n", float64(tot.romR)/float64(tot.ins), float64(tot.ramR)/float64(tot.ins), float64(tot.ramW)/float64(tot.ins), float64(tot.o)/float64(tot.ins))
	fmt.Printf("  callfn 254=%d 255=%d\n", arm.StatCallFn[0], arm.StatCallFn[1])
	fmt.Printf("  dpc+ callfunction:")
	for v, n := range arm.StatDPCPFn {
		if n > 0 {
			fmt.Printf(" %d=%d", v, n)
		}
	}
	fmt.Printf("  fracReads=%d fastFetch=%d\n", arm.StatDPCPFracRead, arm.StatDPCPFastFetch)
	fmt.Printf("  misaligned=%d  recorded calls=%d of %d started\n", arm.StatMisaligned, arm.RecWritten, arm.RecCalls)
	fmt.Printf("  ramExec=%.4f bx=%.4f ldmstm/pushpop=%.4f\n", float64(arm.StatRamExec)/float64(arm.StatInstructions), float64(arm.StatBX)/float64(arm.StatInstructions), float64(arm.StatLdmStm)/float64(arm.StatInstructions))
	fmt.Printf("  fmt:")
	for i, v := range arm.StatFmt {
		if v > 0 {
			fmt.Printf(" %02x=%.3f", i<<3, float64(v)/float64(arm.StatInstructions))
		}
	}
	fmt.Println()
	for k, n := range arm.StatOtherAddr {
		fmt.Printf("  other %c %08x x%d\n", rune(k&0xff), uint32(k>>8), n)
	}
	// code coverage and indirect branches
	nPC := len(arm.StatPCFirst)
	late := map[int]int{}
	for _, f := range arm.StatPCFirst {
		for _, t := range []int{60, 600, 1800, 3600} {
			if f > t {
				late[t]++
			}
		}
	}
	var lo, hi uint32 = 0xffffffff, 0
	for pc := range arm.StatPCFirst {
		if pc < lo {
			lo = pc
		}
		if pc > hi {
			hi = pc
		}
	}
	fmt.Printf("  coverage: %d distinct instr (%d B) span %08x-%08x; first seen after frame 60/600/1800/3600: %d/%d/%d/%d\n",
		nPC, nPC*2, lo, hi, late[60], late[600], late[1800], late[3600])
	sites := map[uint32]map[uint32]bool{}
	for k := range arm.StatIndirect {
		if sites[k[0]] == nil {
			sites[k[0]] = map[uint32]bool{}
		}
		sites[k[0]][k[1]] = true
	}
	multi, targets := 0, map[uint32]bool{}
	for _, t := range sites {
		if len(t) > 1 {
			multi++
		}
		for x := range t {
			targets[x] = true
		}
	}
	fmt.Printf("  indirect: %d sites, %d distinct targets, %d sites with >1 target\n", len(sites), len(targets), multi)
	// split targets into returns (address after a BL) and computed jumps
	compSites, compTargets := map[uint32]bool{}, map[uint32]bool{}
	for k := range arm.StatIndirect {
		if !arm.StatRetAddr[k[1]] {
			compSites[k[0]] = true
			compTargets[k[1]] = true
		}
	}
	fmt.Printf("  computed (not a return): %d sites, %d targets:", len(compSites), len(compTargets))
	for t := range compTargets {
		fmt.Printf(" %x", t)
	}
	fmt.Println()
}
