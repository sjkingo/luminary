#include <string.h>

#include "cpu/dt.h"
#include "cpu/pic.h"
#include "cpu/traps.h"
#include "cpu/x86.h"
#include "kernel/kernel.h"
#include "kernel/sched.h"
#include "kernel/symtab.h"
#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/vmm.h"

#include "drivers/serial.h"

/* The GDT */
struct gdt_entry gdt[GDT_NUM_ENTRIES];
struct gdt_ptr gptr;

/* The IDT */
struct idt_entry idt[256];
struct idt_ptr iptr;

/* The TSS */
static struct tss_entry tss;

static inline void gdt_set_gate(int index, unsigned int base, unsigned int limit,
        unsigned char access, unsigned char granularity)
{
    gdt[index].base_low = (base & 0xFFFF);
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].base_high = (base >> 24) & 0xFF;
    gdt[index].limit_low = (limit & 0xFFFF);
    gdt[index].granularity = (limit >> 16) & 0x0F;
    gdt[index].granularity |= (granularity & 0xF0);
    gdt[index].access = access;
}

static void write_tss(int index, uint32_t ss0, uint32_t esp0)
{
    uint32_t base = (uint32_t)&tss;
    uint32_t limit = base + sizeof(tss) - 1;

    /* TSS descriptor: present, ring 3, type 0x9 (available 32-bit TSS) */
    gdt_set_gate(index, base, limit, 0xE9, 0x00);

    memset(&tss, 0, sizeof(tss));
    tss.ss0 = ss0;
    tss.esp0 = esp0;

    /* Segment selectors the CPU loads when returning to ring 3.
     * These are kernel selectors with RPL=3 set in the low 2 bits. */
    tss.cs = SEG_KERNEL_CODE | 0x3;
    tss.ss = SEG_KERNEL_DATA | 0x3;
    tss.ds = SEG_KERNEL_DATA | 0x3;
    tss.es = SEG_KERNEL_DATA | 0x3;
    tss.fs = SEG_KERNEL_DATA | 0x3;
    tss.gs = SEG_KERNEL_DATA | 0x3;

    /* I/O map base points past the TSS to deny all port access from ring 3 */
    tss.iomap_base = sizeof(tss);
}

void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = esp0;
}

static void gdt_install(void)
{
    gptr.limit = (sizeof(struct gdt_entry) * GDT_NUM_ENTRIES) - 1;
    gptr.base = (unsigned int)&gdt;

    /* Access and granularity bitmasks are described at http://www.osdever.net/bkerndev/Docs/gdt.htm
     *
     * Granularity of 0xCF = 32 bit, 4KB granularity, 0-4GB
     *
     *              32 bits   32 bits     8 bits                8 bits
     *           i  base      limit       access                granularity     */
    gdt_set_gate(0, 0x0,      0x0,        0,                    0);
    gdt_set_gate(1, 0x0,      0xFFFFFFFF, GDT_GATE_KERNEL_CODE, 0xCF);
    gdt_set_gate(2, 0x0,      0xFFFFFFFF, GDT_GATE_KERNEL_DATA, 0xCF);
    gdt_set_gate(3, 0x0,      0xFFFFFFFF, GDT_GATE_USER_CODE,   0xCF);
    gdt_set_gate(4, 0x0,      0xFFFFFFFF, GDT_GATE_USER_DATA,   0xCF);
    write_tss(5, SEG_KERNEL_DATA, 0x0);

    extern void gdt_flush(void);
    extern void tss_flush(void);
    gdt_flush();
    tss_flush();

#ifdef DEBUG
    printk("Installed GDT at %08x (%d entries, TSS at %08lx)\n",
           (unsigned int)&gptr, GDT_NUM_ENTRIES, (uint32_t)&tss);
#endif
}

static inline void idt_set_gate(int index, int vector, int selector, unsigned char flags)
{
    idt[index].base_low = vector & 0xFFFF;
    idt[index].base_high = (vector >> 16) & 0xFFFF;
    idt[index].selector = selector;
    idt[index].always0 = 0;
    idt[index].flags = flags;
}

/* IDT gate flags:
 *   0x8E = present, ring 0, 32-bit interrupt gate
 *   0xEE = present, ring 3, 32-bit interrupt gate (for syscalls)
 */
#define IDT_GATE_RING0  0x8E
#define IDT_GATE_RING3  0xEE

extern int vectors[]; /* in vectors.s */
static void idt_install(void)
{
    iptr.limit = (sizeof(struct idt_entry) * NUM_TRAP_VECTORS) - 1;
    iptr.base = (unsigned int)&idt;

    /* fill in the IDT - all gates default to ring 0 */
    for (int i = 0; i < NUM_TRAP_VECTORS; i++) {
        idt_set_gate(i, vectors[i], IDT_KERNEL_SEG, IDT_GATE_RING0);
    }

    /* syscall gate: int 0x80 must be callable from ring 3 */
    idt_set_gate(SYSCALL_VECTOR, vectors[SYSCALL_VECTOR],
                 IDT_KERNEL_SEG, IDT_GATE_RING3);

    asm volatile("lidt (%0)" : : "r" (&iptr));
#ifdef DEBUG
    printk("Installed IDT at %08x\n", (unsigned int)&iptr);
#endif
}

void init_cpu(void)
{
    if (!in_protected_mode()) {
        panic("cpu is not in protected mode");
    }

    gdt_install();
    idt_install();
    pic_init();
    serial_init();
}

/* Walk the kernel call stack from ebp, printing up to max_frames entries.
 * Stops when EBP is 0 or outside the kernel code range.
 * stack_base and stack_top are the task's kernel stack bounds (for depth annotation). */
static void stack_trace_from(uint32_t ebp, int max_frames,
                              uint32_t stack_base, uint32_t stack_top)
{
    printk("Kernel stack trace:\n");
    for (int i = 0; i < max_frames && ebp != 0; i++) {
        uint32_t *frame = (uint32_t *)ebp;
        uint32_t ret_eip = frame[1];

        if (ret_eip < 0x100000 || ret_eip >= 0xC0000000u)
            break;

        const struct ksym *sym = ksym_lookup(ret_eip);
        if (stack_base && stack_top && ebp >= stack_base && ebp < stack_top) {
            uint32_t depth = stack_top - ebp;
            if (sym)
                printk("  #%d 0x%08lx in %s [+%ld B from top]\n", i, ret_eip, sym->name, depth);
            else
                printk("  #%d 0x%08lx in ??? [+%ld B from top]\n", i, ret_eip, depth);
        } else {
            if (sym)
                printk("  #%d 0x%08lx in %s\n", i, ret_eip, sym->name);
            else
                printk("  #%d 0x%08lx in ???\n", i, ret_eip);
        }

        ebp = frame[0];
    }
    if (stack_base && stack_top) {
        uint32_t hwm = running_task ? running_task->stack_hwm : 0;
        if (hwm && hwm >= stack_base && hwm < stack_top)
            printk("  stack hwm: %ld / %ld bytes used (%ld%%)\n",
                   stack_top - hwm, stack_top - stack_base,
                   (stack_top - hwm) * 100 / (stack_top - stack_base));
    }
}

void dump_trap_frame(struct trap_frame *frame)
{
    printk("\n");
    if (frame->magic != TRAP_MAGIC) {
        printk("BUG: magic=0x%x INVALID -- trap frame may be corrupt.\n", frame->magic);
        return;
    }
    if ((frame->cs & 0x3) == 3)
        printk("User exception in '%s' (pid %d): %d (%s) at %08x\n",
               running_task->name, running_task->pid,
               frame->trapno, VECTOR_NAME(frame->trapno), frame->eip);
    else
        printk("Unhandled exception: %d (%s) at %08x (iret)\n",
               frame->trapno, VECTOR_NAME(frame->trapno), frame->eip);
    printk("ERR=%04x IP=%04x:%08x SP=%04x:%08x GDT=%08x IDT=%08x\n", frame->err,
            frame->cs, frame->eip, frame->ds, frame->esp, (unsigned int)&gptr, (unsigned int)&iptr);
    printk("EAX=%08x EBX=%08x ECX=%08x EDX=%08x [u]ESP=%08x\n", frame->eax, frame->ebx, frame->ecx, frame->edx, frame->uesp);
    printk("ESI=%08x EDI=%08x EBP=%08x ESP=%08x [u]SS =%08x\n", frame->esi, frame->edi, frame->ebp, frame->esp, frame->uss);
    printk("DS=%04x CS=%04x EFLAGS=[ ", frame->ds, frame->cs);
    for (int i = 0; i < 22; i++) {
        if (i == 1 || i == 3 || i == 5 || (i >= 12 && i <= 18)) {
            printk("-");
            continue;
        }
        if (frame->eflags & (1<<i)) {
            printk("1");
        } else {
            printk("0");
        }
    }
    printk(" ] PE=%d PG=%d\n", in_protected_mode(), is_paging_enabled());

    /* Stack context for the running task */
    uint32_t stk_base = 0, stk_top = 0;
    if (running_task) {
        stk_base = running_task->stack_base;
        stk_top  = stk_base + TASK_STACK_SIZE;
        uint32_t esp0 = tss.esp0;
        uint32_t used = stk_top > frame->esp ? stk_top - frame->esp : 0;
        uint32_t hwm_used = running_task->stack_hwm ?
                            stk_top - running_task->stack_hwm : 0;
        printk("STACK base=0x%lx top=0x%lx esp0=0x%lx cur_used=%ld hwm_used=%ld/%ld (%ld%%)\n",
               stk_base, stk_top, esp0,
               used, hwm_used, (uint32_t)TASK_STACK_SIZE,
               hwm_used * 100 / TASK_STACK_SIZE);

        /* Check canary */
        if (stk_base) {
            uint32_t canary = *(volatile uint32_t *)stk_base;
            if (canary != 0xDEADC0DEu)
                printk("CANARY OVERWRITTEN at 0x%lx (was 0x%lx) -- stack overflow!\n",
                       stk_base, canary);
            else
                printk("canary OK\n");
        }
    }
    printk("\n");

    /* For kernel-mode exceptions, walk the kernel EBP chain.
     * For user-mode exceptions, walk the kernel stack we're currently on
     * (frame->ebp is the user EBP which is meaningless here). */
    if ((frame->cs & 0x3) != 3) {
        stack_trace_from(frame->ebp, 16, stk_base, stk_top);
    } else {
        uint32_t cur_ebp;
        asm volatile("mov %%ebp, %0" : "=r"(cur_ebp));
        stack_trace_from(cur_ebp, 16, stk_base, stk_top);
    }
    printk("\n");
}

#define MAX_CONSECUTIVE_FAULTS 5
static int consecutive_faults = 0;

/* Called from syscall.c when a task successfully execs or exits cleanly */
void cpu_reset_fault_counter(void)
{
    consecutive_faults = 0;
}

void trap_handler(struct trap_frame *frame)
{
    if (frame->trapno == SYSCALL_VECTOR) {
        syscall_handler(frame);
        return;
    }

    if (frame->trapno >= IRQ_BASE_OFFSET) {
        /* interrupt from the PIC */
        irq_handler(frame);
        goto out;
    } else {
        /* CPU exception */
        goto exc_handler;
    }

exc_handler:
    /* CoW write fault: handle silently before counting as an error */
    if (frame->trapno == INT_PAGE_FAULT && (frame->err & 0x3) == 0x3) {
        /* err bits: present=1, write=1 — could be a CoW page */
        uint32_t fault_addr;
        asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
        if (vmm_cow_fault(running_task->page_dir_phys, fault_addr))
            goto out;
    }

    consecutive_faults++;
    if (consecutive_faults > MAX_CONSECUTIVE_FAULTS) {
        printk("fault limit reached (%d consecutive faults)\n", consecutive_faults);
        panic("too many consecutive faults");
    }

    dump_trap_frame(frame);

    /* page fault: decode CR2 and error code bits */
    if (frame->trapno == INT_PAGE_FAULT) {
        uint32_t fault_addr;
        asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
        char addr_desc[80];
        vmm_describe_addr(fault_addr, addr_desc, sizeof(addr_desc));
        printk("Page fault at 0x%lx (err=0x%lx) [%s]\n",
               fault_addr, (uint32_t)frame->err, addr_desc);
        if (!(frame->err & 0x1)) printk("  page not present\n");
        if (frame->err & 0x2)    printk("  write access\n");
        if (frame->err & 0x4)    printk("  user mode\n");
    }

    /* if the exception came from user mode (ring 3), kill the task
     * instead of panicking the kernel */
    if ((frame->cs & 0x3) == 3) {
        DBGK("killing '%s' (pid %d)\n",
             running_task->name, running_task->pid);
        disable_interrupts();
        task_kill(running_task);
        /* task_kill does not return when killing running_task */
        __builtin_unreachable();
    }

    /* kernel-mode exception - fatal */
    switch (frame->trapno) {
        case INT_DEBUG:
        case INT_BREAK:
            printk("STOP: %s detected\n", VECTOR_NAME(frame->trapno));
            panic("Stopping kernel execution as requested");

        default:
            panic("CPU exception");
    }

out:
    return;
}
