; SPDX-FileCopyrightText: 2026 HANsoA-KevinO
; SPDX-License-Identifier: MIT
; Win64 ABI-preserving tail forwarding for undocumented/system exports.
; No signatures are guessed: GP/XMM arguments and the caller stack are intact.
EXTERN LabDXGIResolve:PROC
.code
ForwardCommon PROC FRAME
    sub rsp, 88h
    .allocstack 88h
    .endprolog
    mov [rsp+20h], rcx
    mov [rsp+28h], rdx
    mov [rsp+30h], r8
    mov [rsp+38h], r9
    movdqu [rsp+40h], xmm0
    movdqu [rsp+50h], xmm1
    movdqu [rsp+60h], xmm2
    movdqu [rsp+70h], xmm3
    mov ecx, r11d
    call LabDXGIResolve
    mov r11, rax
    mov rcx, [rsp+20h]
    mov rdx, [rsp+28h]
    mov r8, [rsp+30h]
    mov r9, [rsp+38h]
    movdqu xmm0, [rsp+40h]
    movdqu xmm1, [rsp+50h]
    movdqu xmm2, [rsp+60h]
    movdqu xmm3, [rsp+70h]
    add rsp, 88h
    jmp r11
ForwardCommon ENDP
FORWARD MACRO symbol, number
symbol PROC
    mov r11d, number
    jmp ForwardCommon
symbol ENDP
ENDM
FORWARD LabForward0, 0
FORWARD LabForward1, 1
FORWARD LabForward2, 2
FORWARD LabForward6, 6
FORWARD LabForward7, 7
FORWARD LabForward8, 8
FORWARD LabForward9, 9
FORWARD LabForward10, 10
FORWARD LabForward11, 11
FORWARD LabForward12, 12
FORWARD LabForward13, 13
FORWARD LabForward14, 14
FORWARD LabForward15, 15
FORWARD LabForward16, 16
FORWARD LabForward17, 17
FORWARD LabForward19, 19
END
