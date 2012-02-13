# %rdi is unsigned long long length, %rsi is pointer to data
# %rdx (3rd param) is unsigned long long previous...
.globl rehash_mul
.type  rehash_mul,@function
rehash_mul:
    movabs  $0xac95a395a9ec535b, %r8
    mov     %rsi, %rax
    xor     %r8, %rax
    xor     %rdx, %rax
    sub     $0x8, %rsi
    jb      2f
.align 8, 0x90
1:  xor     (%rdi), %rax
    mul     %r8
    add     $0x8, %rdi
    xor     %rsi, %rax
    xor     %rdx, %rax
    sub     $0x8, %rsi
    jae     1b
2:  cmp     $-0x8, %rsi
    jne     4f
3:  mov     %rax, %rsi
    mul     %r8
    xor     %rsi, %rax
    xor     %rdx, %rax
    mul     %r8
    add     %rsi, %rax
    add     %rdx, %rax
    retq
4:  xor     %rdx, %rdx
    test    $0x4, %rsi
    je      5f
    mov     (%rdi), %edx
    add     $0x4, %rdi
5:  test    $0x2, %rsi
    je      6f
    movzwl  (%rdi), %ecx
    shl     $0x10, %rdx
    add     %rcx, %rdx
    add     $0x2, %rdi
6:  test    $0x1, %rsi
    je      7f
    movzbl  (%rdi), %ecx
    shl     $0x8, %rdx
    add     %rcx, %rdx
7:  xor     %rdx, %rax
    mul     %r8
    xor     %rdx, %rax
    jmp 3b
.size   rehash_mul, .-rehash_mul
