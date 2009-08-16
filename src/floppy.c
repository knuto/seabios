// 16bit code to access floppy drives.
//
// Copyright (C) 2008,2009  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2002  MandrakeSoft S.A.
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "types.h" // u8
#include "disk.h" // DISK_RET_SUCCESS
#include "config.h" // CONFIG_FLOPPY
#include "biosvar.h" // SET_BDA
#include "util.h" // irq_disable
#include "cmos.h" // inb_cmos
#include "pic.h" // eoi_pic1
#include "bregs.h" // struct bregs

#define FLOPPY_SECTOR_SIZE 512

#define BX_FLOPPY_ON_CNT 37   /* 2 seconds */

// New diskette parameter table adding 3 parameters from IBM
// Since no provisions are made for multiple drive types, most
// values in this table are ignored.  I set parameters for 1.44M
// floppy here
struct floppy_ext_dbt_s diskette_param_table2 VAR16_32 = {
    .dbt = {
        .specify1       = 0xAF,
        .specify2       = 0x02, // head load time 0000001, DMA used
        .shutoff_ticks  = 0x25,
        .bps_code       = 0x02,
        .sectors        = 18,
        .interblock_len = 0x1B,
        .data_len       = 0xFF,
        .gap_len        = 0x6C,
        .fill_byte      = 0xF6,
        .settle_time    = 0x0F,
        .startup_time   = 0x08,
    },
    .max_track      = 79,   // maximum track
    .data_rate      = 0,    // data transfer rate
    .drive_type     = 4,    // drive type in cmos
};

// Since no provisions are made for multiple drive types, most
// values in this table are ignored.  I set parameters for 1.44M
// floppy here
struct floppy_dbt_s diskette_param_table VAR16FIXED(0xefc7) = {
    .specify1       = 0xAF,
    .specify2       = 0x02, // head load time 0000001, DMA used
    .shutoff_ticks  = 0x25,
    .bps_code       = 0x02,
    .sectors        = 18,
    .interblock_len = 0x1B,
    .data_len       = 0xFF,
    .gap_len        = 0x6C,
    .fill_byte      = 0xF6,
    .settle_time    = 0x0F,
    .startup_time   = 0x08,
};

struct floppyinfo_s {
    struct chs_s chs;
    u8 config_data;
    u8 media_state;
};

struct floppyinfo_s FloppyInfo[] VAR16_32 = {
    // Unknown
    { {0, 0, 0}, 0x00, 0x00},
    // 1 - 360KB, 5.25" - 2 heads, 40 tracks, 9 sectors
    { {2, 40, 9}, 0x00, 0x25},
    // 2 - 1.2MB, 5.25" - 2 heads, 80 tracks, 15 sectors
    { {2, 80, 15}, 0x00, 0x25},
    // 3 - 720KB, 3.5"  - 2 heads, 80 tracks, 9 sectors
    { {2, 80, 9}, 0x00, 0x17},
    // 4 - 1.44MB, 3.5" - 2 heads, 80 tracks, 18 sectors
    { {2, 80, 18}, 0x00, 0x17},
    // 5 - 2.88MB, 3.5" - 2 heads, 80 tracks, 36 sectors
    { {2, 80, 36}, 0xCC, 0xD7},
    // 6 - 160k, 5.25"  - 1 heads, 40 tracks, 8 sectors
    { {1, 40, 8}, 0x00, 0x27},
    // 7 - 180k, 5.25"  - 1 heads, 40 tracks, 9 sectors
    { {1, 40, 9}, 0x00, 0x27},
    // 8 - 320k, 5.25"  - 2 heads, 40 tracks, 8 sectors
    { {2, 40, 8}, 0x00, 0x27},
};

static void
addFloppy(int floppyid, int ftype)
{
    if (ftype <= 0 || ftype >= ARRAY_SIZE(FloppyInfo)) {
        dprintf(1, "Bad floppy type %d\n", ftype);
        return;
    }

    int driveid = Drives.drivecount;
    if (driveid >= ARRAY_SIZE(Drives.drives))
        return;
    Drives.drivecount++;
    memset(&Drives.drives[driveid], 0, sizeof(Drives.drives[0]));
    Drives.drives[driveid].cntl_id = floppyid;
    Drives.drives[driveid].type = DTYPE_FLOPPY;
    Drives.drives[driveid].blksize = FLOPPY_SECTOR_SIZE;
    Drives.drives[driveid].floppy_type = ftype;
    Drives.drives[driveid].sectors = (u16)-1;

    memcpy(&Drives.drives[driveid].lchs, &FloppyInfo[ftype].chs
           , sizeof(FloppyInfo[ftype].chs));

    map_floppy_drive(driveid);
}

void
floppy_setup()
{
    if (! CONFIG_FLOPPY)
        return;
    dprintf(3, "init floppy drives\n");

    if (CONFIG_COREBOOT) {
        // XXX - disable floppies on coreboot for now.
    } else {
        u8 type = inb_cmos(CMOS_FLOPPY_DRIVE_TYPE);
        if (type & 0xf0)
            addFloppy(0, type >> 4);
        if (type & 0x0f)
            addFloppy(1, type & 0x0f);
    }

    outb(0x02, PORT_DMA1_MASK_REG);

    enable_hwirq(6, entry_0e);
}


/****************************************************************
 * Low-level floppy IO
 ****************************************************************/

static void
floppy_reset_controller()
{
    // Reset controller
    u8 val8 = inb(PORT_FD_DOR);
    outb(val8 & ~0x04, PORT_FD_DOR);
    outb(val8 | 0x04, PORT_FD_DOR);

    // Wait for controller to come out of reset
    while ((inb(PORT_FD_STATUS) & 0xc0) != 0x80)
        ;
}

static int
wait_floppy_irq()
{
    irq_enable();
    u8 v;
    for (;;) {
        if (!GET_BDA(floppy_motor_counter)) {
            irq_disable();
            return -1;
        }
        v = GET_BDA(floppy_recalibration_status);
        if (v & FRS_TIMEOUT)
            break;
        cpu_relax();
    }
    irq_disable();

    v &= ~FRS_TIMEOUT;
    SET_BDA(floppy_recalibration_status, v);
    return 0;
}

static void
floppy_prepare_controller(u8 floppyid)
{
    CLEARBITS_BDA(floppy_recalibration_status, FRS_TIMEOUT);

    // turn on motor of selected drive, DMA & int enabled, normal operation
    u8 prev_reset = inb(PORT_FD_DOR) & 0x04;
    u8 dor = 0x10;
    if (floppyid)
        dor = 0x20;
    dor |= 0x0c;
    dor |= floppyid;
    outb(dor, PORT_FD_DOR);

    // reset the disk motor timeout value of INT 08
    SET_BDA(floppy_motor_counter, BX_FLOPPY_ON_CNT);

    // wait for drive readiness
    while ((inb(PORT_FD_STATUS) & 0xc0) != 0x80)
        ;

    if (!prev_reset)
        wait_floppy_irq();
}

static int
floppy_pio(u8 *cmd, u8 cmdlen)
{
    floppy_prepare_controller(cmd[1] & 1);

    // send command to controller
    u8 i;
    for (i=0; i<cmdlen; i++)
        outb(cmd[i], PORT_FD_DATA);

    int ret = wait_floppy_irq();
    if (ret) {
        floppy_reset_controller();
        return -1;
    }

    return 0;
}

static int
floppy_cmd(struct disk_op_s *op, u16 count, u8 *cmd, u8 cmdlen)
{
    // es:bx = pointer to where to place information from diskette
    u32 addr = (u32)op->buf_fl;

    // check for 64K boundary overrun
    u32 last_addr = addr + count;
    if ((addr >> 16) != (last_addr >> 16))
        return DISK_RET_EBOUNDARY;

    u8 mode_register = 0x4a; // single mode, increment, autoinit disable,
    if (cmd[0] == 0xe6)
        // read
        mode_register = 0x46;

    //DEBUGF("floppy dma c2\n");
    outb(0x06, PORT_DMA1_MASK_REG);
    outb(0x00, PORT_DMA1_CLEAR_FF_REG); // clear flip-flop
    outb(addr, PORT_DMA_ADDR_2);
    outb(addr>>8, PORT_DMA_ADDR_2);
    outb(0x00, PORT_DMA1_CLEAR_FF_REG); // clear flip-flop
    outb(count, PORT_DMA_CNT_2);
    outb(count>>8, PORT_DMA_CNT_2);

    // port 0b: DMA-1 Mode Register
    // transfer type=write, channel 2
    outb(mode_register, PORT_DMA1_MODE_REG);

    // port 81: DMA-1 Page Register, channel 2
    outb(addr>>16, PORT_DMA_PAGE_2);

    outb(0x02, PORT_DMA1_MASK_REG); // unmask channel 2

    int ret = floppy_pio(cmd, cmdlen);
    if (ret)
        return DISK_RET_ETIMEOUT;

    // check port 3f4 for accessibility to status bytes
    if ((inb(PORT_FD_STATUS) & 0xc0) != 0xc0)
        return DISK_RET_ECONTROLLER;

    // read 7 return status bytes from controller
    u8 i;
    for (i=0; i<7; i++) {
        u8 v = inb(PORT_FD_DATA);
        cmd[i] = v;
        SET_BDA(floppy_return_status[i], v);
    }

    return DISK_RET_SUCCESS;
}


/****************************************************************
 * Floppy media sense
 ****************************************************************/

static inline void
set_diskette_current_cyl(u8 floppyid, u8 cyl)
{
    SET_BDA(floppy_track[floppyid], cyl);
}

static void
floppy_drive_recal(u8 floppyid)
{
    // send Recalibrate command (2 bytes) to controller
    u8 data[12];
    data[0] = 0x07;  // 07: Recalibrate
    data[1] = floppyid; // 0=drive0, 1=drive1
    floppy_pio(data, 2);

    SETBITS_BDA(floppy_recalibration_status, 1<<floppyid);
    set_diskette_current_cyl(floppyid, 0);
}

static int
floppy_media_sense(u8 driveid)
{
    // for now cheat and get drive type from CMOS,
    // assume media is same as drive type

    // ** config_data **
    // Bitfields for diskette media control:
    // Bit(s)  Description (Table M0028)
    //  7-6  last data rate set by controller
    //        00=500kbps, 01=300kbps, 10=250kbps, 11=1Mbps
    //  5-4  last diskette drive step rate selected
    //        00=0Ch, 01=0Dh, 10=0Eh, 11=0Ah
    //  3-2  {data rate at start of operation}
    //  1-0  reserved

    // ** media_state **
    // Bitfields for diskette drive media state:
    // Bit(s)  Description (Table M0030)
    //  7-6  data rate
    //    00=500kbps, 01=300kbps, 10=250kbps, 11=1Mbps
    //  5  double stepping required (e.g. 360kB in 1.2MB)
    //  4  media type established
    //  3  drive capable of supporting 4MB media
    //  2-0  on exit from BIOS, contains
    //    000 trying 360kB in 360kB
    //    001 trying 360kB in 1.2MB
    //    010 trying 1.2MB in 1.2MB
    //    011 360kB in 360kB established
    //    100 360kB in 1.2MB established
    //    101 1.2MB in 1.2MB established
    //    110 reserved
    //    111 all other formats/drives

    u8 ftype = GET_GLOBAL(Drives.drives[driveid].floppy_type);
    SET_BDA(floppy_last_data_rate, GET_GLOBAL(FloppyInfo[ftype].config_data));
    u8 floppyid = GET_GLOBAL(Drives.drives[driveid].cntl_id);
    SET_BDA(floppy_media_state[floppyid]
            , GET_GLOBAL(FloppyInfo[ftype].media_state));
    return DISK_RET_SUCCESS;
}

static int
check_recal_drive(u8 driveid)
{
    u8 floppyid = GET_GLOBAL(Drives.drives[driveid].cntl_id);
    if ((GET_BDA(floppy_recalibration_status) & (1<<floppyid))
        && (GET_BDA(floppy_media_state[floppyid]) & FMS_MEDIA_DRIVE_ESTABLISHED))
        // Media is known.
        return DISK_RET_SUCCESS;

    // Recalibrate drive.
    floppy_drive_recal(floppyid);

    // Sense media.
    return floppy_media_sense(driveid);
}


/****************************************************************
 * Floppy handlers
 ****************************************************************/

static void
lba2chs(struct disk_op_s *op, u8 *track, u8 *sector, u8 *head)
{
    u32 lba = op->lba;
    u8 driveid = op->driveid;

    u32 tmp = lba + 1;
    u16 nlspt = GET_GLOBAL(Drives.drives[driveid].lchs.spt);
    *sector = tmp % nlspt;

    tmp /= nlspt;
    u16 nlh = GET_GLOBAL(Drives.drives[driveid].lchs.heads);
    *head = tmp % nlh;

    tmp /= nlh;
    *track = tmp;
}

// diskette controller reset
static int
floppy_reset(struct disk_op_s *op)
{
    u8 floppyid = GET_GLOBAL(Drives.drives[op->driveid].cntl_id);
    set_diskette_current_cyl(floppyid, 0); // current cylinder
    return DISK_RET_SUCCESS;
}

// Read Diskette Sectors
static int
floppy_read(struct disk_op_s *op)
{
    int res = check_recal_drive(op->driveid);
    if (res)
        goto fail;

    u8 track, sector, head;
    lba2chs(op, &track, &sector, &head);

    // send read-normal-data command (9 bytes) to controller
    u8 floppyid = GET_GLOBAL(Drives.drives[op->driveid].cntl_id);
    u8 data[12];
    data[0] = 0xe6; // e6: read normal data
    data[1] = (head << 2) | floppyid; // HD DR1 DR2
    data[2] = track;
    data[3] = head;
    data[4] = sector;
    data[5] = 2; // 512 byte sector size
    data[6] = sector + op->count - 1; // last sector to read on track
    data[7] = 0; // Gap length
    data[8] = 0xff; // Gap length

    res = floppy_cmd(op, (op->count * FLOPPY_SECTOR_SIZE) - 1, data, 9);
    if (res)
        goto fail;

    if (data[0] & 0xc0) {
        res = DISK_RET_ECONTROLLER;
        goto fail;
    }

    // ??? should track be new val from return_status[3] ?
    set_diskette_current_cyl(floppyid, track);
    return DISK_RET_SUCCESS;
fail:
    op->count = 0; // no sectors read
    return res;
}

// Write Diskette Sectors
static int
floppy_write(struct disk_op_s *op)
{
    int res = check_recal_drive(op->driveid);
    if (res)
        goto fail;

    u8 track, sector, head;
    lba2chs(op, &track, &sector, &head);

    // send write-normal-data command (9 bytes) to controller
    u8 floppyid = GET_GLOBAL(Drives.drives[op->driveid].cntl_id);
    u8 data[12];
    data[0] = 0xc5; // c5: write normal data
    data[1] = (head << 2) | floppyid; // HD DR1 DR2
    data[2] = track;
    data[3] = head;
    data[4] = sector;
    data[5] = 2; // 512 byte sector size
    data[6] = sector + op->count - 1; // last sector to write on track
    data[7] = 0; // Gap length
    data[8] = 0xff; // Gap length

    res = floppy_cmd(op, (op->count * FLOPPY_SECTOR_SIZE) - 1, data, 9);
    if (res)
        goto fail;

    if (data[0] & 0xc0) {
        if (data[1] & 0x02)
            res = DISK_RET_EWRITEPROTECT;
        else
            res = DISK_RET_ECONTROLLER;
        goto fail;
    }

    // ??? should track be new val from return_status[3] ?
    set_diskette_current_cyl(floppyid, track);
    return DISK_RET_SUCCESS;
fail:
    op->count = 0; // no sectors read
    return res;
}

// Verify Diskette Sectors
static int
floppy_verify(struct disk_op_s *op)
{
    int res = check_recal_drive(op->driveid);
    if (res)
        goto fail;

    u8 track, sector, head;
    lba2chs(op, &track, &sector, &head);

    // ??? should track be new val from return_status[3] ?
    u8 floppyid = GET_GLOBAL(Drives.drives[op->driveid].cntl_id);
    set_diskette_current_cyl(floppyid, track);
    return DISK_RET_SUCCESS;
fail:
    op->count = 0; // no sectors read
    return res;
}

// format diskette track
static int
floppy_format(struct disk_op_s *op)
{
    int ret = check_recal_drive(op->driveid);
    if (ret)
        return ret;

    u8 head = op->lba;

    // send format-track command (6 bytes) to controller
    u8 floppyid = GET_GLOBAL(Drives.drives[op->driveid].cntl_id);
    u8 data[12];
    data[0] = 0x4d; // 4d: format track
    data[1] = (head << 2) | floppyid; // HD DR1 DR2
    data[2] = 2; // 512 byte sector size
    data[3] = op->count; // number of sectors per track
    data[4] = 0; // Gap length
    data[5] = 0xf6; // Fill byte

    ret = floppy_cmd(op, (op->count * 4) - 1, data, 6);
    if (ret)
        return ret;

    if (data[0] & 0xc0) {
        if (data[1] & 0x02)
            return DISK_RET_EWRITEPROTECT;
        return DISK_RET_ECONTROLLER;
    }

    set_diskette_current_cyl(floppyid, 0);
    return DISK_RET_SUCCESS;
}

int
process_floppy_op(struct disk_op_s *op)
{
    if (!CONFIG_FLOPPY)
        return 0;

    switch (op->command) {
    case CMD_RESET:
        return floppy_reset(op);
    case CMD_READ:
        return floppy_read(op);
    case CMD_WRITE:
        return floppy_write(op);
    case CMD_VERIFY:
        return floppy_verify(op);
    case CMD_FORMAT:
        return floppy_format(op);
    default:
        op->count = 0;
        return DISK_RET_EPARAM;
    }
}


/****************************************************************
 * HW irqs
 ****************************************************************/

// INT 0Eh Diskette Hardware ISR Entry Point
void VISIBLE16
handle_0e()
{
    debug_isr(DEBUG_ISR_0e);
    if (! CONFIG_FLOPPY)
        goto done;

    if ((inb(PORT_FD_STATUS) & 0xc0) != 0xc0) {
        outb(0x08, PORT_FD_DATA); // sense interrupt status
        while ((inb(PORT_FD_STATUS) & 0xc0) != 0xc0)
            ;
        do {
            inb(PORT_FD_DATA);
        } while ((inb(PORT_FD_STATUS) & 0xc0) == 0xc0);
    }
    // diskette interrupt has occurred
    SETBITS_BDA(floppy_recalibration_status, FRS_TIMEOUT);

done:
    eoi_pic1();
}

// Called from int08 handler.
void
floppy_tick()
{
    if (! CONFIG_FLOPPY)
        return;

    // time to turn off drive(s)?
    u8 fcount = GET_BDA(floppy_motor_counter);
    if (fcount) {
        fcount--;
        SET_BDA(floppy_motor_counter, fcount);
        if (fcount == 0)
            // turn motor(s) off
            outb(inb(PORT_FD_DOR) & 0xcf, PORT_FD_DOR);
    }
}
