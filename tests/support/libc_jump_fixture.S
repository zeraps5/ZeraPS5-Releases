/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Assembly source for libc_jump_fixture.h, System V amd64, no relocations.
 * Rebuild with gcc -c, then objcopy -O binary -j .text. */
.text
.intel_syntax noprefix
push rbp
mov rbp,rsp
push rbx
push r12
push r13
push r14
push r15
sub rsp,8
mov r15,rdi
mov [r15+104],rbp
mov rbx,0x1122
mov r12,0x3344
mov r13,0x5566
mov r14,0x7788
mov rdi,[r15+16]
call [r15]
test eax,eax
jnz resumed
mov edi,3
mov rsi,[r15+80]
xor edx,edx
call [r15+72]
ldmxcsr [r15+96]
fldcw [r15+124]
xor ebx,ebx
xor r12d,r12d
xor r13d,r13d
xor r14d,r14d
xor ebp,ebp
mov rdi,[r15+16]
mov esi,[r15+24]
call [r15+8]
ud2
resumed:
mov [r15+32],rax
mov [r15+40],rbx
mov [r15+48],r12
mov [r15+56],r13
mov [r15+64],r14
mov [r15+112],rbp
stmxcsr [r15+88]
fnstcw [r15+120]
xor eax,eax
add rsp,8
pop r15
pop r14
pop r13
pop r12
pop rbx
pop rbp
ret
