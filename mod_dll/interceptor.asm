.code

EXTERN g_magnesisEnabled: byte
EXTERN g_magneHeartbeatCounter: qword
EXTERN g_magneIdealBase: qword
EXTERN g_magnesisZWriterReturn: qword
EXTERN g_magnesisYWriterReturn: qword

AsmMagnesisZWriter PROC
    pushf
    push rax
    lea rax, [r13 + rbp + 68h]
    mov [g_magneIdealBase], rax
    pop rax
    inc qword ptr [g_magneHeartbeatCounter]
    cmp byte ptr [g_magnesisEnabled], 1
    je SkipNativeWrite
    movbe [r13 + rbp + 70h], r14d

SkipNativeWrite:
    cvtss2sd xmm9, xmm9
    movddup xmm9, xmm9

    popf
    jmp [g_magnesisZWriterReturn]
AsmMagnesisZWriter ENDP

AsmMagnesisYWriterExp PROC
    pushf
    push rax
    lea rax, [r13 + rdx]
    mov [g_magneIdealBase], rax
    pop rax
    inc qword ptr [g_magneHeartbeatCounter]
    cmp byte ptr [g_magnesisEnabled], 1
    je SkipNativeWrite
    movbe [r13 + rdx + 04h], r14d

SkipNativeWrite:
    cvtss2sd xmm0, xmm0
    movbe r14d, [r13 + rbx + 08h]

    popf
    jmp [g_magnesisYWriterReturn]
AsmMagnesisYWriterExp ENDP

AsmMagnesisZWriterExp PROC
    pushf
    push rax
    lea rax, [r13 + rbp]
    mov [g_magneIdealBase], rax
    pop rax
    inc qword ptr [g_magneHeartbeatCounter]
    cmp byte ptr [g_magnesisEnabled], 1
    je SkipNativeWrite
    movbe [r13 + rbp + 70h], r14d

SkipNativeWrite:
    popf
    movsd qword ptr [rsp + 148h], xmm6
    movd r14d, xmm6
    jmp [g_magnesisZWriterReturn]
AsmMagnesisZWriterExp ENDP

END
