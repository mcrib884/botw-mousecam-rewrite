.code

EXTERN g_magnesisEnabled: byte
EXTERN g_magneHeartbeatCounter: qword
EXTERN g_magneIdealBase: qword
EXTERN g_magnesisZWriterReturn: qword

AsmMagnesisZWriter PROC
    pushf
    push rax
    lea rax, [r13 + rbp + 68h]
    mov [g_magneIdealBase], rax
    pop rax
    inc qword ptr [g_magneHeartbeatCounter]
    cmp byte ptr [g_magnesisEnabled], 1
    je SkipNativeWrite
    movbe [r13 + rbp + 70h], r9d

SkipNativeWrite:
    cvtss2sd xmm9, xmm9
    movddup xmm9, xmm9

    popf
    jmp [g_magnesisZWriterReturn]
AsmMagnesisZWriter ENDP

END
