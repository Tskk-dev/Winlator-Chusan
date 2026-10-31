/* Keep every guest register/flag deterministic across the fixture API ABI. */
.text
.globl _oracle_enter
_oracle_enter:
    pushfl
    pushal
    movl %esp, _oracle_host_sp
    pushl _oracle_input+32
    popfl
    movl _oracle_input+16, %esp
    movl _oracle_input+4, %ecx
    movl _oracle_input+8, %edx
    movl _oracle_input+12, %ebx
    movl _oracle_input+20, %ebp
    movl _oracle_input+24, %esi
    movl _oracle_input+28, %edi
    movl _oracle_input, %eax
    jmp *L_target
L_target: .long 0x00e3d42f
.globl _oracle_done
_oracle_done:
    movl %eax, _oracle_result
    movl %ecx, _oracle_result+4
    movl %edx, _oracle_result+8
    movl %ebx, _oracle_result+12
    movl %esp, _oracle_result+16
    movl %ebp, _oracle_result+20
    movl %esi, _oracle_result+24
    movl %edi, _oracle_result+28
    pushfl
    popl _oracle_result+32
    cld
    movl _oracle_host_sp, %esp
    popal
    popfl
    ret

.macro fixture name, id, adjustment
.globl \name
\name:
    pushfl
    pushal
    cld
    leal 36(%esp), %eax
    pushl %eax
    pushl $\id
    call _oracle_fixture
    addl $8, %esp
    movl %eax, 28(%esp)
    popal
    popfl
    ret $\adjustment
.endm
fixture _oracle_api0, 0, 4
fixture _oracle_api1, 1, 0
fixture _oracle_api2, 2, 0
fixture _oracle_api3, 3, 4
