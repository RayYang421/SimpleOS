.section .text
.global _start
.type _start, %function
_start:
    b _start
    .size _start, . - _start
