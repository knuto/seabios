// 16bit code to handle system clocks.
//
// Copyright (C) 2008-2010  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2002  MandrakeSoft S.A.
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "biosvar.h" // SET_BDA
#include "util.h" // debug_enter
#include "disk.h" // floppy_tick
#include "hw/cmos.h" // inb_cmos
#include "hw/pic.h" // pic_eoi1
#include "hw/pit.h" // PM_SEL_TIMER0
#include "bregs.h" // struct bregs
#include "biosvar.h" // GET_GLOBAL
#include "hw/usb-hid.h" // usb_check_event

// RTC register flags
#define RTC_A_UIP 0x80

#define RTC_B_SET  0x80
#define RTC_B_PIE  0x40
#define RTC_B_AIE  0x20
#define RTC_B_UIE  0x10
#define RTC_B_BIN  0x04
#define RTC_B_24HR 0x02
#define RTC_B_DSE  0x01


/****************************************************************
 * Init
 ****************************************************************/

static int
rtc_updating(void)
{
    // This function checks to see if the update-in-progress bit
    // is set in CMOS Status Register A.  If not, it returns 0.
    // If it is set, it tries to wait until there is a transition
    // to 0, and will return 0 if such a transition occurs.  A -1
    // is returned only after timing out.  The maximum period
    // that this bit should be set is constrained to (1984+244)
    // useconds, but we wait for longer just to be sure.

    if ((inb_cmos(CMOS_STATUS_A) & RTC_A_UIP) == 0)
        return 0;
    u32 end = timer_calc(15);
    for (;;) {
        if ((inb_cmos(CMOS_STATUS_A) & RTC_A_UIP) == 0)
            return 0;
        if (timer_check(end))
            // update-in-progress never transitioned to 0
            return -1;
        yield();
    }
}

static void
pit_setup(void)
{
    // timer0: binary count, 16bit count, mode 2
    outb(PM_SEL_TIMER0|PM_ACCESS_WORD|PM_MODE2|PM_CNT_BINARY, PORT_PIT_MODE);
    // maximum count of 0000H = 18.2Hz
    outb(0x0, PORT_PIT_COUNTER0);
    outb(0x0, PORT_PIT_COUNTER0);
}

static void
rtc_setup(void)
{
    outb_cmos(0x26, CMOS_STATUS_A);    // 32,768Khz src, 976.5625us updates
    u8 regB = inb_cmos(CMOS_STATUS_B);
    outb_cmos((regB & RTC_B_DSE) | RTC_B_24HR, CMOS_STATUS_B);
    inb_cmos(CMOS_STATUS_C);
    inb_cmos(CMOS_STATUS_D);
}

static u32
bcd2bin(u8 val)
{
    return (val & 0xf) + ((val >> 4) * 10);
}

u8 Century VARLOW;

void
clock_setup(void)
{
    dprintf(3, "init timer\n");
    pit_setup();

    rtc_setup();
    rtc_updating();
    u32 seconds = bcd2bin(inb_cmos(CMOS_RTC_SECONDS));
    u32 minutes = bcd2bin(inb_cmos(CMOS_RTC_MINUTES));
    u32 hours = bcd2bin(inb_cmos(CMOS_RTC_HOURS));
    u32 ticks = ticks_from_ms(((hours * 60 + minutes) * 60 + seconds) * 1000);
    SET_BDA(timer_counter, ticks % TICKS_PER_DAY);

    // Setup Century storage
    if (CONFIG_QEMU) {
        Century = inb_cmos(CMOS_CENTURY);
    } else {
        // Infer current century from the year.
        u8 year = inb_cmos(CMOS_RTC_YEAR);
        if (year > 0x80)
            Century = 0x19;
        else
            Century = 0x20;
    }

    enable_hwirq(0, FUNC16(entry_08));
    enable_hwirq(8, FUNC16(entry_70));
}


/****************************************************************
 * Standard clock functions
 ****************************************************************/

// get current clock count
static void
handle_1a00(struct bregs *regs)
{
    yield();
    u32 ticks = GET_BDA(timer_counter);
    regs->cx = ticks >> 16;
    regs->dx = ticks;
    regs->al = GET_BDA(timer_rollover);
    SET_BDA(timer_rollover, 0); // reset flag
    set_success(regs);
}

// Set Current Clock Count
static void
handle_1a01(struct bregs *regs)
{
    u32 ticks = (regs->cx << 16) | regs->dx;
    SET_BDA(timer_counter, ticks);
    SET_BDA(timer_rollover, 0); // reset flag
    // XXX - should use set_code_success()?
    regs->ah = 0;
    set_success(regs);
}

// Read CMOS Time
static void
handle_1a02(struct bregs *regs)
{
    if (rtc_updating()) {
        set_invalid(regs);
        return;
    }

    regs->dh = inb_cmos(CMOS_RTC_SECONDS);
    regs->cl = inb_cmos(CMOS_RTC_MINUTES);
    regs->ch = inb_cmos(CMOS_RTC_HOURS);
    regs->dl = inb_cmos(CMOS_STATUS_B) & RTC_B_DSE;
    regs->ah = 0;
    regs->al = regs->ch;
    set_success(regs);
}

// Set CMOS Time
static void
handle_1a03(struct bregs *regs)
{
    // Using a debugger, I notice the following masking/setting
    // of bits in Status Register B, by setting Reg B to
    // a few values and getting its value after INT 1A was called.
    //
    //        try#1       try#2       try#3
    // before 1111 1101   0111 1101   0000 0000
    // after  0110 0010   0110 0010   0000 0010
    //
    // Bit4 in try#1 flipped in hardware (forced low) due to bit7=1
    // My assumption: RegB = ((RegB & 01100000b) | 00000010b)
    if (rtc_updating()) {
        rtc_setup();
        // fall through as if an update were not in progress
    }
    outb_cmos(regs->dh, CMOS_RTC_SECONDS);
    outb_cmos(regs->cl, CMOS_RTC_MINUTES);
    outb_cmos(regs->ch, CMOS_RTC_HOURS);
    // Set Daylight Savings time enabled bit to requested value
    u8 val8 = ((inb_cmos(CMOS_STATUS_B) & (RTC_B_PIE|RTC_B_AIE))
               | RTC_B_24HR | (regs->dl & RTC_B_DSE));
    outb_cmos(val8, CMOS_STATUS_B);
    regs->ah = 0;
    regs->al = val8; // val last written to Reg B
    set_success(regs);
}

// Read CMOS Date
static void
handle_1a04(struct bregs *regs)
{
    regs->ah = 0;
    if (rtc_updating()) {
        set_invalid(regs);
        return;
    }
    regs->cl = inb_cmos(CMOS_RTC_YEAR);
    regs->dh = inb_cmos(CMOS_RTC_MONTH);
    regs->dl = inb_cmos(CMOS_RTC_DAY_MONTH);
    regs->ch = GET_LOW(Century);
    regs->al = regs->ch;
    set_success(regs);
}

// Set CMOS Date
static void
handle_1a05(struct bregs *regs)
{
    // Using a debugger, I notice the following masking/setting
    // of bits in Status Register B, by setting Reg B to
    // a few values and getting its value after INT 1A was called.
    //
    //        try#1       try#2       try#3       try#4
    // before 1111 1101   0111 1101   0000 0010   0000 0000
    // after  0110 1101   0111 1101   0000 0010   0000 0000
    //
    // Bit4 in try#1 flipped in hardware (forced low) due to bit7=1
    // My assumption: RegB = (RegB & 01111111b)
    if (rtc_updating()) {
        rtc_setup();
        set_invalid(regs);
        return;
    }
    outb_cmos(regs->cl, CMOS_RTC_YEAR);
    outb_cmos(regs->dh, CMOS_RTC_MONTH);
    outb_cmos(regs->dl, CMOS_RTC_DAY_MONTH);
    SET_LOW(Century, regs->ch);
    // clear halt-clock bit
    u8 val8 = inb_cmos(CMOS_STATUS_B) & ~RTC_B_SET;
    outb_cmos(val8, CMOS_STATUS_B);
    regs->ah = 0;
    regs->al = val8; // AL = val last written to Reg B
    set_success(regs);
}

// Set Alarm Time in CMOS
static void
handle_1a06(struct bregs *regs)
{
    // Using a debugger, I notice the following masking/setting
    // of bits in Status Register B, by setting Reg B to
    // a few values and getting its value after INT 1A was called.
    //
    //        try#1       try#2       try#3
    // before 1101 1111   0101 1111   0000 0000
    // after  0110 1111   0111 1111   0010 0000
    //
    // Bit4 in try#1 flipped in hardware (forced low) due to bit7=1
    // My assumption: RegB = ((RegB & 01111111b) | 00100000b)
    u8 val8 = inb_cmos(CMOS_STATUS_B); // Get Status Reg B
    regs->ax = 0;
    if (val8 & RTC_B_AIE) {
        // Alarm interrupt enabled already
        set_invalid(regs);
        return;
    }
    if (rtc_updating()) {
        rtc_setup();
        // fall through as if an update were not in progress
    }
    outb_cmos(regs->dh, CMOS_RTC_SECONDS_ALARM);
    outb_cmos(regs->cl, CMOS_RTC_MINUTES_ALARM);
    outb_cmos(regs->ch, CMOS_RTC_HOURS_ALARM);
    // enable Status Reg B alarm bit, clear halt clock bit
    outb_cmos((val8 & ~RTC_B_SET) | RTC_B_AIE, CMOS_STATUS_B);
    set_success(regs);
}

// Turn off Alarm
static void
handle_1a07(struct bregs *regs)
{
    // Using a debugger, I notice the following masking/setting
    // of bits in Status Register B, by setting Reg B to
    // a few values and getting its value after INT 1A was called.
    //
    //        try#1       try#2       try#3       try#4
    // before 1111 1101   0111 1101   0010 0000   0010 0010
    // after  0100 0101   0101 0101   0000 0000   0000 0010
    //
    // Bit4 in try#1 flipped in hardware (forced low) due to bit7=1
    // My assumption: RegB = (RegB & 01010111b)
    u8 val8 = inb_cmos(CMOS_STATUS_B); // Get Status Reg B
    // clear clock-halt bit, disable alarm bit
    outb_cmos(val8 & ~(RTC_B_SET|RTC_B_AIE), CMOS_STATUS_B);
    regs->ah = 0;
    regs->al = val8; // val last written to Reg B
    set_success(regs);
}

// Unsupported
static void
handle_1aXX(struct bregs *regs)
{
    set_unimplemented(regs);
}

// INT 1Ah Time-of-day Service Entry Point
void VISIBLE16
handle_1a(struct bregs *regs)
{
    debug_enter(regs, DEBUG_HDL_1a);
    switch (regs->ah) {
    case 0x00: handle_1a00(regs); break;
    case 0x01: handle_1a01(regs); break;
    case 0x02: handle_1a02(regs); break;
    case 0x03: handle_1a03(regs); break;
    case 0x04: handle_1a04(regs); break;
    case 0x05: handle_1a05(regs); break;
    case 0x06: handle_1a06(regs); break;
    case 0x07: handle_1a07(regs); break;
    default:   handle_1aXX(regs); break;
    }
}

// INT 08h System Timer ISR Entry Point
void VISIBLE16
handle_08(void)
{
    debug_isr(DEBUG_ISR_08);

    // Update counter
    u32 counter = GET_BDA(timer_counter);
    counter++;
    // compare to one days worth of timer ticks at 18.2 hz
    if (counter >= TICKS_PER_DAY) {
        // there has been a midnight rollover at this point
        counter = 0;
        SET_BDA(timer_rollover, GET_BDA(timer_rollover) + 1);
    }
    SET_BDA(timer_counter, counter);

    // Check for internal events.
    floppy_tick();
    usb_check_event();

    // chain to user timer tick INT #0x1c
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.flags = F_IF;
    call16_int(0x1c, &br);

    pic_eoi1();
}


/****************************************************************
 * Periodic timer
 ****************************************************************/

int RTCusers VARLOW;

void
useRTC(void)
{
    int count = GET_LOW(RTCusers);
    SET_LOW(RTCusers, count+1);
    if (count)
        return;
    // Turn on the Periodic Interrupt timer
    u8 bRegister = inb_cmos(CMOS_STATUS_B);
    outb_cmos(bRegister | RTC_B_PIE, CMOS_STATUS_B);
}

void
releaseRTC(void)
{
    int count = GET_LOW(RTCusers);
    SET_LOW(RTCusers, count-1);
    if (count != 1)
        return;
    // Clear the Periodic Interrupt.
    u8 bRegister = inb_cmos(CMOS_STATUS_B);
    outb_cmos(bRegister & ~RTC_B_PIE, CMOS_STATUS_B);
}

static int
set_usertimer(u32 usecs, u16 seg, u16 offset)
{
    if (GET_BDA(rtc_wait_flag) & RWS_WAIT_PENDING)
        return -1;

    // Interval not already set.
    SET_BDA(rtc_wait_flag, RWS_WAIT_PENDING);  // Set status byte.
    SET_BDA(user_wait_complete_flag, SEGOFF(seg, offset));
    SET_BDA(user_wait_timeout, usecs);
    useRTC();
    return 0;
}

static void
clear_usertimer(void)
{
    if (!(GET_BDA(rtc_wait_flag) & RWS_WAIT_PENDING))
        return;
    // Turn off status byte.
    SET_BDA(rtc_wait_flag, 0);
    releaseRTC();
}

#define RET_ECLOCKINUSE  0x83

// Wait for CX:DX microseconds
void
handle_1586(struct bregs *regs)
{
    // Use the rtc to wait for the specified time.
    u8 statusflag = 0;
    u32 count = (regs->cx << 16) | regs->dx;
    int ret = set_usertimer(count, GET_SEG(SS), (u32)&statusflag);
    if (ret) {
        set_code_invalid(regs, RET_ECLOCKINUSE);
        return;
    }
    while (!statusflag)
        yield_toirq();
    set_success(regs);
}

// Set Interval requested.
static void
handle_158300(struct bregs *regs)
{
    int ret = set_usertimer((regs->cx << 16) | regs->dx, regs->es, regs->bx);
    if (ret)
        // Interval already set.
        set_code_invalid(regs, RET_EUNSUPPORTED);
    else
        set_success(regs);
}

// Clear interval requested
static void
handle_158301(struct bregs *regs)
{
    clear_usertimer();
    set_success(regs);
}

static void
handle_1583XX(struct bregs *regs)
{
    set_code_unimplemented(regs, RET_EUNSUPPORTED);
    regs->al--;
}

void
handle_1583(struct bregs *regs)
{
    switch (regs->al) {
    case 0x00: handle_158300(regs); break;
    case 0x01: handle_158301(regs); break;
    default:   handle_1583XX(regs); break;
    }
}

#define USEC_PER_RTC DIV_ROUND_CLOSEST(1000000, 1024)

// int70h: IRQ8 - CMOS RTC
void VISIBLE16
handle_70(void)
{
    debug_isr(DEBUG_ISR_70);

    // Check which modes are enabled and have occurred.
    u8 registerB = inb_cmos(CMOS_STATUS_B);
    u8 registerC = inb_cmos(CMOS_STATUS_C);

    if (!(registerB & (RTC_B_PIE|RTC_B_AIE)))
        goto done;
    if (registerC & RTC_B_AIE) {
        // Handle Alarm Interrupt.
        struct bregs br;
        memset(&br, 0, sizeof(br));
        br.flags = F_IF;
        call16_int(0x4a, &br);
    }
    if (!(registerC & RTC_B_PIE))
        goto done;

    // Handle Periodic Interrupt.

    check_preempt();

    if (!GET_BDA(rtc_wait_flag))
        goto done;

    // Wait Interval (Int 15, AH=83) active.
    u32 time = GET_BDA(user_wait_timeout);  // Time left in microseconds.
    if (time < USEC_PER_RTC) {
        // Done waiting - write to specified flag byte.
        struct segoff_s segoff = GET_BDA(user_wait_complete_flag);
        u16 ptr_seg = segoff.seg;
        u8 *ptr_far = (u8*)(segoff.offset+0);
        u8 oldval = GET_FARVAR(ptr_seg, *ptr_far);
        SET_FARVAR(ptr_seg, *ptr_far, oldval | 0x80);

        clear_usertimer();
    } else {
        // Continue waiting.
        time -= USEC_PER_RTC;
        SET_BDA(user_wait_timeout, time);
    }

done:
    pic_eoi2();
}
