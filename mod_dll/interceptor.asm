.code

EXTERN g_magnesisEnabled: byte
EXTERN g_magneHeartbeatCounter: qword
EXTERN g_magneIdealBase: qword
EXTERN g_magnesisXWriterReturn: qword

AsmMagnesisXWriter PROC
    pushf
    push rax
    lea rax, [r13 + rbp + 68h]
    mov [g_magneIdealBase], rax
    pop rax
    inc qword ptr [g_magneHeartbeatCounter]
    cmp byte ptr [g_magnesisEnabled], 1
    je SkipNativeWrite
    push rbx
    mov ebx, r14d
    bswap ebx
    mov [r13 + rbp + 68h], ebx
    pop rbx

SkipNativeWrite:
    cvtss2sd xmm6, xmm6
    movddup xmm6, xmm6

    popf
    jmp [g_magnesisXWriterReturn]
AsmMagnesisXWriter ENDP

END
