When linking the kernel you may receive the following linker warning:

    /bin/ld: warning: .note.gnu.build-id section discarded, --build-id ignored.

This is safe to ignore. The linker script instructs the linker to discard
this section as it is a) not needed and b) bloats the kernel binary, which
has a bad side effect of causing the Multiboot header to be placed at an
offset >8K.
