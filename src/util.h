// Basic x86 asm functions and function defs.
//
// Copyright (C) 2008-2010  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU LGPLv3 license.
#ifndef __UTIL_H
#define __UTIL_H

#include "types.h" // u32

static inline void irq_disable(void)
{
    asm volatile("cli": : :"memory");
}

static inline void irq_enable(void)
{
    asm volatile("sti": : :"memory");
}

static inline u32 save_flags(void)
{
    u32 flags;
    asm volatile("pushfl ; popl %0" : "=rm" (flags));
    return flags;
}

static inline void restore_flags(u32 flags)
{
    asm volatile("pushl %0 ; popfl" : : "g" (flags) : "memory", "cc");
}

static inline void cpu_relax(void)
{
    asm volatile("rep ; nop": : :"memory");
}

static inline void nop(void)
{
    asm volatile("nop");
}

static inline void hlt(void)
{
    asm volatile("hlt": : :"memory");
}

static inline void wbinvd(void)
{
    asm volatile("wbinvd": : :"memory");
}

#define CPUID_TSC (1 << 4)
#define CPUID_MSR (1 << 5)
#define CPUID_APIC (1 << 9)
#define CPUID_MTRR (1 << 12)
static inline void __cpuid(u32 index, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    asm("cpuid"
        : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
        : "0" (index));
}

static inline u32 getcr0(void) {
    u32 cr0;
    asm("movl %%cr0, %0" : "=r"(cr0));
    return cr0;
}
static inline void setcr0(u32 cr0) {
    asm("movl %0, %%cr0" : : "r"(cr0));
}

static inline u64 rdmsr(u32 index)
{
    u64 ret;
    asm ("rdmsr" : "=A"(ret) : "c"(index));
    return ret;
}

static inline void wrmsr(u32 index, u64 val)
{
    asm volatile ("wrmsr" : : "c"(index), "A"(val));
}

static inline u64 rdtscll(void)
{
    u64 val;
    asm volatile("rdtsc" : "=A" (val));
    return val;
}

static inline u32 __ffs(u32 word)
{
    asm("bsf %1,%0"
        : "=r" (word)
        : "rm" (word));
    return word;
}
static inline u32 __fls(u32 word)
{
    asm("bsr %1,%0"
        : "=r" (word)
        : "rm" (word));
    return word;
}

static inline u32 getesp(void) {
    u32 esp;
    asm("movl %%esp, %0" : "=rm"(esp));
    return esp;
}

static inline void writel(void *addr, u32 val) {
    *(volatile u32 *)addr = val;
}
static inline void writew(void *addr, u16 val) {
    *(volatile u16 *)addr = val;
}
static inline void writeb(void *addr, u8 val) {
    *(volatile u8 *)addr = val;
}
static inline u32 readl(const void *addr) {
    return *(volatile const u32 *)addr;
}
static inline u16 readw(const void *addr) {
    return *(volatile const u16 *)addr;
}
static inline u8 readb(const void *addr) {
    return *(volatile const u8 *)addr;
}

// GDT bits
#define GDT_CODE     (0x9bULL << 40) // Code segment - P,R,A bits also set
#define GDT_DATA     (0x93ULL << 40) // Data segment - W,A bits also set
#define GDT_B        (0x1ULL << 54)  // Big flag
#define GDT_G        (0x1ULL << 55)  // Granularity flag
// GDT bits for segment base
#define GDT_BASE(v)  ((((u64)(v) & 0xff000000) << 32)           \
                      | (((u64)(v) & 0x00ffffff) << 16))
// GDT bits for segment limit (0-1Meg)
#define GDT_LIMIT(v) ((((u64)(v) & 0x000f0000) << 32)   \
                      | (((u64)(v) & 0x0000ffff) << 0))
// GDT bits for segment limit (0-4Gig in 4K chunks)
#define GDT_GRANLIMIT(v) (GDT_G | GDT_LIMIT((v) >> 12))

struct descloc_s {
    u16 length;
    u32 addr;
} PACKED;

// util.c
void cpuid(u32 index, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx);
u8 checksum_far(u16 buf_seg, void *buf_far, u32 len);
u8 checksum(void *buf, u32 len);
size_t strlen(const char *s);
int memcmp(const void *s1, const void *s2, size_t n);
int strcmp(const char *s1, const char *s2);
inline void memset_far(u16 d_seg, void *d_far, u8 c, size_t len);
inline void memset16_far(u16 d_seg, void *d_far, u16 c, size_t len);
void *memset(void *s, int c, size_t n);
void memset_fl(void *ptr, u8 val, size_t size);
inline void memcpy_far(u16 d_seg, void *d_far
                       , u16 s_seg, const void *s_far, size_t len);
void memcpy_fl(void *d_fl, const void *s_fl, size_t len);
void *memcpy(void *d1, const void *s1, size_t len);
#if MODESEGMENT == 0
#define memcpy __builtin_memcpy
#endif
void iomemcpy(void *d, const void *s, u32 len);
void *memmove(void *d, const void *s, size_t len);
char *strtcpy(char *dest, const char *src, size_t len);
char *strchr(const char *s, int c);
void nullTrailingSpace(char *buf);
int get_keystroke(int msec);

// stacks.c
extern u8 ExtraStack[], *StackPos;
u32 stack_hop(u32 eax, u32 edx, void *func);
u32 stack_hop_back(u32 eax, u32 edx, void *func);
u32 call32(void *func, u32 eax, u32 errret);
struct bregs;
inline void farcall16(struct bregs *callregs);
inline void farcall16big(struct bregs *callregs);
inline void __call16_int(struct bregs *callregs, u16 offset);
#define call16_int(nr, callregs) do {                           \
        extern void irq_trampoline_ ##nr ();                    \
        __call16_int((callregs), (u32)&irq_trampoline_ ##nr );  \
    } while (0)
extern struct thread_info MainThread;
struct thread_info *getCurThread(void);
void yield(void);
void yield_toirq(void);
void run_thread(void (*func)(void*), void *data);
void wait_threads(void);
struct mutex_s { u32 isLocked; };
void mutex_lock(struct mutex_s *mutex);
void mutex_unlock(struct mutex_s *mutex);
void start_preempt(void);
void finish_preempt(void);
int wait_preempt(void);
void check_preempt(void);

// output.c
extern u16 DebugOutputPort;
void debug_serial_preinit(void);
void panic(const char *fmt, ...)
    __attribute__ ((format (printf, 1, 2))) __noreturn;
void printf(const char *fmt, ...)
    __attribute__ ((format (printf, 1, 2)));
int snprintf(char *str, size_t size, const char *fmt, ...)
    __attribute__ ((format (printf, 3, 4)));
char * znprintf(size_t size, const char *fmt, ...)
    __attribute__ ((format (printf, 2, 3)));
void __dprintf(const char *fmt, ...)
    __attribute__ ((format (printf, 1, 2)));
void __debug_enter(struct bregs *regs, const char *fname);
void __debug_isr(const char *fname);
void __debug_stub(struct bregs *regs, int lineno, const char *fname);
void __warn_invalid(struct bregs *regs, int lineno, const char *fname);
void __warn_unimplemented(struct bregs *regs, int lineno, const char *fname);
void __warn_internalerror(int lineno, const char *fname);
void __warn_noalloc(int lineno, const char *fname);
void __warn_timeout(int lineno, const char *fname);
void __set_invalid(struct bregs *regs, int lineno, const char *fname);
void __set_unimplemented(struct bregs *regs, int lineno, const char *fname);
void __set_code_invalid(struct bregs *regs, u32 linecode, const char *fname);
void __set_code_unimplemented(struct bregs *regs, u32 linecode
                              , const char *fname);
void hexdump(const void *d, int len);

#define dprintf(lvl, fmt, args...) do {                         \
        if (CONFIG_DEBUG_LEVEL && (lvl) <= CONFIG_DEBUG_LEVEL)  \
            __dprintf((fmt) , ##args );                         \
    } while (0)
#define debug_enter(regs, lvl) do {                     \
        if ((lvl) && (lvl) <= CONFIG_DEBUG_LEVEL)       \
            __debug_enter((regs), __func__);            \
    } while (0)
#define debug_isr(lvl) do {                             \
        if ((lvl) && (lvl) <= CONFIG_DEBUG_LEVEL)       \
            __debug_isr(__func__);                      \
    } while (0)
#define debug_stub(regs)                        \
    __debug_stub((regs), __LINE__, __func__)
#define warn_invalid(regs)                      \
    __warn_invalid((regs), __LINE__, __func__)
#define warn_unimplemented(regs)                        \
    __warn_unimplemented((regs), __LINE__, __func__)
#define warn_internalerror()                    \
    __warn_internalerror(__LINE__, __func__)
#define warn_noalloc()                          \
    __warn_noalloc(__LINE__, __func__)
#define warn_timeout()                          \
    __warn_timeout(__LINE__, __func__)
#define set_invalid(regs)                       \
    __set_invalid((regs), __LINE__, __func__)
#define set_code_invalid(regs, code)                                    \
    __set_code_invalid((regs), (code) | (__LINE__ << 8), __func__)
#define set_unimplemented(regs)                         \
    __set_unimplemented((regs), __LINE__, __func__)
#define set_code_unimplemented(regs, code)                              \
    __set_code_unimplemented((regs), (code) | (__LINE__ << 8), __func__)

// kbd.c
void kbd_init(void);
void handle_15c2(struct bregs *regs);
void process_key(u8 key);

// mouse.c
void mouse_init(void);
void process_mouse(u8 data);

// serial.c
void serial_setup(void);
void lpt_setup(void);

// clock.c
void clock_setup(void);
void handle_1583(struct bregs *regs);
void handle_1586(struct bregs *regs);
void useRTC(void);
void releaseRTC(void);

// hw/timer.c
void timer_setup(void);
void pmtimer_setup(u16 ioport);
u32 timer_calc(u32 msecs);
u32 timer_calc_usec(u32 usecs);
int timer_check(u32 end);
void ndelay(u32 count);
void udelay(u32 count);
void mdelay(u32 count);
void nsleep(u32 count);
void usleep(u32 count);
void msleep(u32 count);
u32 ticks_to_ms(u32 ticks);
u32 ticks_from_ms(u32 ms);
u32 irqtimer_calc_ticks(u32 count);
u32 irqtimer_calc(u32 msecs);
int irqtimer_check(u32 end);

// apm.c
void apm_shutdown(void);
void handle_1553(struct bregs *regs);

// pcibios.c
void handle_1ab1(struct bregs *regs);
void bios32_init(void);

// fw/shadow.c
void make_bios_writable(void);
void make_bios_readonly(void);
void qemu_prep_reset(void);

// fw/pciinit.c
extern const u8 pci_irqs[4];
void pci_setup(void);

// fw/smm.c
void smm_device_setup(void);
void smm_setup(void);

// fw/smp.c
extern u32 CountCPUs;
extern u32 MaxCountCPUs;
void wrmsr_smp(u32 index, u64 val);
void smp_setup(void);
int apic_id_is_present(u8 apic_id);

// fw/coreboot.c
extern const char *CBvendor, *CBpart;
struct cbfs_file;
void debug_cbmem(char c);
void cbfs_run_payload(struct cbfs_file *file);
void coreboot_platform_setup(void);
void cbfs_payload_setup(void);
void coreboot_preinit(void);
void coreboot_cbfs_init(void);

// fw/biostable.c
void copy_smbios(void *pos);
void copy_table(void *pos);

// vgahooks.c
void handle_155f(struct bregs *regs);
struct pci_device;
void vgahook_setup(struct pci_device *pci);

// optionroms.c
void call_bcv(u16 seg, u16 ip);
int is_pci_vga(struct pci_device *pci);
void optionrom_setup(void);
void vgarom_setup(void);
void s3_resume_vga(void);
extern int ScreenAndDebug;

// bootsplash.c
void enable_vga_console(void);
void enable_bootsplash(void);
void disable_bootsplash(void);

// resume.c
extern int HaveRunPost;
void dma_setup(void);

// pnpbios.c
#define PNP_SIGNATURE 0x506e5024 // $PnP
u16 get_pnp_offset(void);
void pnp_init(void);

// pmm.c
extern struct zone_s ZoneLow, ZoneHigh, ZoneFSeg, ZoneTmpLow, ZoneTmpHigh;
u32 rom_get_max(void);
u32 rom_get_last(void);
struct rom_header *rom_reserve(u32 size);
int rom_confirm(u32 size);
void csm_malloc_preinit(u32 low_pmm, u32 low_pmm_size, u32 hi_pmm,
                        u32 hi_pmm_size);
void malloc_preinit(void);
extern u32 LegacyRamSize;
void malloc_init(void);
void malloc_prepboot(void);
void *pmm_malloc(struct zone_s *zone, u32 handle, u32 size, u32 align);
int pmm_free(void *data);
void pmm_init(void);
void pmm_prepboot(void);
#define PMM_DEFAULT_HANDLE 0xFFFFFFFF
// Minimum alignment of malloc'd memory
#define MALLOC_MIN_ALIGN 16
// Helper functions for memory allocation.
static inline void *malloc_low(u32 size) {
    return pmm_malloc(&ZoneLow, PMM_DEFAULT_HANDLE, size, MALLOC_MIN_ALIGN);
}
static inline void *malloc_high(u32 size) {
    return pmm_malloc(&ZoneHigh, PMM_DEFAULT_HANDLE, size, MALLOC_MIN_ALIGN);
}
static inline void *malloc_fseg(u32 size) {
    return pmm_malloc(&ZoneFSeg, PMM_DEFAULT_HANDLE, size, MALLOC_MIN_ALIGN);
}
static inline void *malloc_tmplow(u32 size) {
    return pmm_malloc(&ZoneTmpLow, PMM_DEFAULT_HANDLE, size, MALLOC_MIN_ALIGN);
}
static inline void *malloc_tmphigh(u32 size) {
    return pmm_malloc(&ZoneTmpHigh, PMM_DEFAULT_HANDLE, size, MALLOC_MIN_ALIGN);
}
static inline void *malloc_tmp(u32 size) {
    void *ret = malloc_tmphigh(size);
    if (ret)
        return ret;
    return malloc_tmplow(size);
}
static inline void *memalign_low(u32 align, u32 size) {
    return pmm_malloc(&ZoneLow, PMM_DEFAULT_HANDLE, size, align);
}
static inline void *memalign_high(u32 align, u32 size) {
    return pmm_malloc(&ZoneHigh, PMM_DEFAULT_HANDLE, size, align);
}
static inline void *memalign_tmplow(u32 align, u32 size) {
    return pmm_malloc(&ZoneTmpLow, PMM_DEFAULT_HANDLE, size, align);
}
static inline void *memalign_tmphigh(u32 align, u32 size) {
    return pmm_malloc(&ZoneTmpHigh, PMM_DEFAULT_HANDLE, size, align);
}
static inline void *memalign_tmp(u32 align, u32 size) {
    void *ret = memalign_tmphigh(align, size);
    if (ret)
        return ret;
    return memalign_tmplow(align, size);
}
static inline void free(void *data) {
    pmm_free(data);
}

// fw/mtrr.c
void mtrr_setup(void);

// romfile.c
struct romfile_s {
    struct romfile_s *next;
    char name[128];
    u32 size;
    int (*copy)(struct romfile_s *file, void *dest, u32 maxlen);
};
void romfile_add(struct romfile_s *file);
struct romfile_s *romfile_findprefix(const char *prefix, struct romfile_s *prev);
struct romfile_s *romfile_find(const char *name);
void *romfile_loadfile(const char *name, int *psize);
u64 romfile_loadint(const char *name, u64 defval);

// romlayout.S
void reset_vector(void) __noreturn;

// misc.c
void mathcp_setup(void);
extern u8 BiosChecksum;

// version (auto generated file out/version.c)
extern const char VERSION[];

#endif // util.h
