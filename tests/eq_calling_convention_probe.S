/*
 * Synthetic x86 ABI reproduction; no Windows or AIMP execution is claimed.
 *
 * The official Delphi SDK declares GetActive without stdcall, so its Self
 * argument uses EAX and the callee does not pop a stack Self argument. The
 * C++ header declares WINAPI/stdcall and therefore expects that pop. This
 * probe demonstrates the four-byte imbalance and contrasts the documented
 * stdcall message-dispatcher signature used by the 0.2.1 workaround.
 *
 * Linux x86/x86_64 build, without 32-bit C runtime dependencies:
 * cc -m32 -nostdlib -static -no-pie tests/eq_calling_convention_probe.S -o build/eq-abi-probe
 * ./build/eq-abi-probe
 */

.section .text
.global _start
_start:
    movl %esp, %esi
    movl $0x12345678, %eax
    pushl %eax
    call delphi_register_get_active
    /* stdcall caller expects ESP == ESI, but the register callee leaves Self. */
    leal 4(%esp), %edx
    cmpl %esi, %edx
    jne failed
    cmpl $1, %eax
    jne failed
    movl %esi, %esp

    /* Send(Self, EQUALIZER, GET, BOOL*): all four stdcall parameters are popped. */
    subl $4, %esp
    movl $0, (%esp)
    movl %esp, %edi
    movl %esp, %esi
    pushl %edi
    pushl $0
    pushl $0x100e
    pushl $0x12345678
    call stdcall_property_get
    cmpl %esi, %esp
    jne failed
    testl %eax, %eax
    jne failed
    cmpl $1, (%edi)
    jne failed

    movl $4, %eax
    movl $1, %ebx
    movl $success_message, %ecx
    movl $success_length, %edx
    int $0x80
    movl $1, %eax
    xorl %ebx, %ebx
    int $0x80

failed:
    movl $1, %eax
    movl $1, %ebx
    int $0x80

delphi_register_get_active:
    cmpl $0x12345678, %eax
    jne failed
    movl $1, %eax
    ret

stdcall_property_get:
    cmpl $0x12345678, 4(%esp)
    jne failed
    cmpl $0x100e, 8(%esp)
    jne failed
    cmpl $0, 12(%esp)
    jne failed
    movl 16(%esp), %edx
    movl $1, (%edx)
    xorl %eax, %eax
    ret $16

.section .rodata
success_message:
    .ascii "x86 ABI probe: register/stdcall mismatch leaves 4 bytes; dispatcher call balances stack\n"
.set success_length, . - success_message

.section .note.GNU-stack,"",@progbits
