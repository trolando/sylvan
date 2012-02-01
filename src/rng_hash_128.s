.align 16
.globl rng_hash_128
.type  rng_hash_128,@function
rng_hash_128:
	mov    8(%rdi), %rax
	
	movabs $0x6595a395a1ec531b, %rcx

#   Option 1)
#   Non-threaded, fastest.  No xor instruction used.

#   Option 2)
#   Threaded, pass the nonce as a second parameter.
#	xor    %rsi, %rax

#   Option 3)
#   Threaded, use the address of the seed as a nonce.
#   xor    %rdi, %rax  
	
	mov    (%rdi), %rsi
	mul    %rcx
	
	add    %rcx, (%rdi)
	adc    %rsi, 8(%rdi)
	
	xor    %rsi, %rax
	xor    %rdx, %rax
	mul    %rcx
	add    %rsi, %rax
	add    %rdx, %rax
	
	ret
.size	rng_hash_128, .-rng_hash_128
