// Internal timer support.
//
// Copyright (C) 2008-2013  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "util.h" // dprintf
#include "pit.h" // PM_SEL_TIMER0
#include "ioport.h" // PORT_PIT_MODE
#include "config.h" // CONFIG_*
#include "biosvar.h" // GET_LOW

// Bits for PORT_PS2_CTRLB
#define PPCB_T2GATE (1<<0)
#define PPCB_SPKR   (1<<1)
#define PPCB_T2OUT  (1<<5)

#define PMTIMER_HZ 3579545      // Underlying Hz of the PM Timer
#define PMTIMER_TO_PIT 3        // Ratio of pmtimer rate to pit rate

u32 TimerKHz VARFSEG;
u16 TimerPort VARFSEG;
u8 ShiftTSC VARFSEG;


/****************************************************************
 * Timer setup
 ****************************************************************/

#define CALIBRATE_COUNT 0x800   // Approx 1.7ms

// Calibrate the CPU time-stamp-counter
static void
tsctimer_setup(void)
{
    // Setup "timer2"
    u8 orig = inb(PORT_PS2_CTRLB);
    outb((orig & ~PPCB_SPKR) | PPCB_T2GATE, PORT_PS2_CTRLB);
    /* binary, mode 0, LSB/MSB, Ch 2 */
    outb(PM_SEL_TIMER2|PM_ACCESS_WORD|PM_MODE0|PM_CNT_BINARY, PORT_PIT_MODE);
    /* LSB of ticks */
    outb(CALIBRATE_COUNT & 0xFF, PORT_PIT_COUNTER2);
    /* MSB of ticks */
    outb(CALIBRATE_COUNT >> 8, PORT_PIT_COUNTER2);

    u64 start = rdtscll();
    while ((inb(PORT_PS2_CTRLB) & PPCB_T2OUT) == 0)
        ;
    u64 end = rdtscll();

    // Restore PORT_PS2_CTRLB
    outb(orig, PORT_PS2_CTRLB);

    // Store calibrated cpu khz.
    u64 diff = end - start;
    dprintf(6, "tsc calibrate start=%u end=%u diff=%u\n"
            , (u32)start, (u32)end, (u32)diff);
    u64 t = DIV_ROUND_UP(diff * PMTIMER_HZ, CALIBRATE_COUNT);
    while (t >= (1<<24)) {
        ShiftTSC++;
        t = (t + 1) >> 1;
    }
    TimerKHz = DIV_ROUND_UP((u32)t, 1000 * PMTIMER_TO_PIT);

    dprintf(1, "CPU Mhz=%u\n", (TimerKHz << ShiftTSC) / 1000);
}

// Setup internal timers.
void
timer_setup(void)
{
    if (CONFIG_PMTIMER && TimerPort) {
        dprintf(3, "pmtimer already configured; will not calibrate TSC\n");
        return;
    }

    u32 eax, ebx, ecx, edx, cpuid_features = 0;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    if (eax > 0)
        cpuid(1, &eax, &ebx, &ecx, &cpuid_features);

    if (!(cpuid_features & CPUID_TSC)) {
        TimerPort = PORT_PIT_COUNTER0;
        TimerKHz = DIV_ROUND_UP(PMTIMER_HZ, 1000 * PMTIMER_TO_PIT);
        dprintf(3, "386/486 class CPU. Using TSC emulation\n");
        return;
    }

    tsctimer_setup();
}

void
pmtimer_setup(u16 ioport)
{
    if (!CONFIG_PMTIMER)
        return;
    dprintf(1, "Using pmtimer, ioport 0x%x\n", ioport);
    TimerPort = ioport;
    TimerKHz = DIV_ROUND_UP(PMTIMER_HZ, 1000);
}


/****************************************************************
 * Internal timer reading
 ****************************************************************/

u32 TimerLast VARLOW;

// Add extra high bits to timers that have less than 32bits of precision.
static u32
timer_adjust_bits(u32 value, u32 validbits)
{
    u32 last = GET_LOW(TimerLast);
    value = (last & ~validbits) | (value & validbits);
    if (value < last)
        value += validbits + 1;
    SET_LOW(TimerLast, value);
    return value;
}

// Sample the current timer value.
static u32
timer_read(void)
{
    u16 port = GET_GLOBAL(TimerPort);
    if (!port)
        // Read from CPU TSC
        return rdtscll() >> GET_GLOBAL(ShiftTSC);
    if (CONFIG_PMTIMER && port != PORT_PIT_COUNTER0)
        // Read from PMTIMER
        return timer_adjust_bits(inl(port), 0xffffff);
    // Read from PIT.
    outb(PM_SEL_READBACK | PM_READ_VALUE | PM_READ_COUNTER0, PORT_PIT_MODE);
    u16 v = inb(PORT_PIT_COUNTER0) | (inb(PORT_PIT_COUNTER0) << 8);
    return timer_adjust_bits(v, 0xffff);
}

// Check if the current time is past a previously calculated end time.
int
timer_check(u32 end)
{
    return (s32)(timer_read() - end) > 0;
}

static void
timer_delay(u32 diff)
{
    u32 start = timer_read();
    u32 end = start + diff;
    while (!timer_check(end))
        cpu_relax();
}

static void
timer_sleep(u32 diff)
{
    u32 start = timer_read();
    u32 end = start + diff;
    while (!timer_check(end))
        yield();
}

void ndelay(u32 count) {
    timer_delay(DIV_ROUND_UP(count * GET_GLOBAL(TimerKHz), 1000000));
}
void udelay(u32 count) {
    timer_delay(DIV_ROUND_UP(count * GET_GLOBAL(TimerKHz), 1000));
}
void mdelay(u32 count) {
    timer_delay(count * GET_GLOBAL(TimerKHz));
}

void nsleep(u32 count) {
    timer_sleep(DIV_ROUND_UP(count * GET_GLOBAL(TimerKHz), 1000000));
}
void usleep(u32 count) {
    timer_sleep(DIV_ROUND_UP(count * GET_GLOBAL(TimerKHz), 1000));
}
void msleep(u32 count) {
    timer_sleep(count * GET_GLOBAL(TimerKHz));
}

// Return the TSC value that is 'msecs' time in the future.
u32
timer_calc(u32 msecs)
{
    return timer_read() + (GET_GLOBAL(TimerKHz) * msecs);
}
u32
timer_calc_usec(u32 usecs)
{
    return timer_read() + DIV_ROUND_UP(GET_GLOBAL(TimerKHz) * usecs, 1000);
}


/****************************************************************
 * IRQ based timer
 ****************************************************************/

#define PIT_TICK_INTERVAL 65536 // Default interval for 18.2Hz timer

// Return the number of milliseconds in 'ticks' number of timer irqs.
u32
ticks_to_ms(u32 ticks)
{
    u32 t = PIT_TICK_INTERVAL * 1000 * PMTIMER_TO_PIT * ticks;
    return DIV_ROUND_UP(t, PMTIMER_HZ);
}

// Return the number of timer irqs in 'ms' number of milliseconds.
u32
ticks_from_ms(u32 ms)
{
    u32 t = DIV_ROUND_UP((u64)ms * PMTIMER_HZ, PIT_TICK_INTERVAL);
    return DIV_ROUND_UP(t, 1000 * PMTIMER_TO_PIT);
}

// Calculate the timer value at 'count' number of full timer ticks in
// the future.
u32
irqtimer_calc_ticks(u32 count)
{
    return (GET_BDA(timer_counter) + count + 1) % TICKS_PER_DAY;
}

// Return the timer value that is 'msecs' time in the future.
u32
irqtimer_calc(u32 msecs)
{
    if (!msecs)
        return GET_BDA(timer_counter);
    return irqtimer_calc_ticks(ticks_from_ms(msecs));
}

// Check if the given timer value has passed.
int
irqtimer_check(u32 end)
{
    return (((GET_BDA(timer_counter) + TICKS_PER_DAY - end) % TICKS_PER_DAY)
            < (TICKS_PER_DAY/2));
}
