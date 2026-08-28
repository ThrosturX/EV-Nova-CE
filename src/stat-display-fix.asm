%include "macros/patch.inc"

; Fix ship speed values displaying one below their nominal value
; due to floating-point quantization followed by truncation.
;
; For example, ship Speed 190 is stored internally as approximately
; 1.899999976, which becomes approximately 189.9999976 when converted
; back to display units. The existing FIST already rounds this to 190;
; skip the following truncation correction and preserve that result.

; shipyard speed display
@SJMP 0x00495F62, 0x00495F79

; Player Info converts float-derived movement stats to integers using
; x87 extended precision and a long truncation sequence. Values that
; should reconstruct to exact integers can therefore land just below
; them (e.g. 74.999998 -> 74).
;
; Quantize the scaled value back to Nova's float32 precision first,
; then truncate that float directly. Genuine fractions remain truncated.

; Turning
@PATCH 0x0049AF49
    fstp dword [esp+0x924]
    cvttss2si ecx, dword [esp+0x924]
    db 0xEB, 0x3E ; jmp rel8 to push ecx
@ENDPATCH

; Acceleration
@PATCH 0x0049B052
    fstp dword [esp+0x924]
    cvttss2si ebp, dword [esp+0x924]
    db 0xEB, 0x39
@ENDPATCH

; Speed (strict mode)
@PATCH 0x0049B14E
    fstp dword [esp+0x924]
    cvttss2si ebx, dword [esp+0x924]
    db 0xEB, 0x39
@ENDPATCH

; Speed (non-strict mode)
@PATCH 0x0049B1C0
    fstp dword [esp+0x924]
    cvttss2si ecx, dword [esp+0x924]
    db 0xEB, 0x39
@ENDPATCH
