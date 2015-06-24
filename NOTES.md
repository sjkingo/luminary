# Notes

When linking the kernel you may receive the following linker warning:

    /bin/ld: warning: .note.gnu.build-id section discarded, --build-id ignored.

This is safe to ignore. The linker script instructs the linker to discard
this section as it is a) not needed and b) bloats the kernel binary, which
has a bad side effect of causing the Multiboot header to be placed at an
offset >8K.

---

The kernel requires a minimum of 2 MB of memory available to boot. This is
simply a technical requirement as the kernel is mapped to physical address
1 MB at boot (and should take up no more than 1 MB of space).

If required, this can be changed by altering the linker script and (possibly)
the stack setup code in `boot.s`.

---

The kernel uses a flat memory model addressable up to 4 GB physical. There is
no concept of a virtual memory system, and all memory is directly accessible by
its physical address.

Physical memory is divided into two segments: kernel code (ro) and kernel data
(rw). This is enforced by the CPU through the GDT.
