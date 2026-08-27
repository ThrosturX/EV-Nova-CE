%include "macros/patch.inc"

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
@ENDPATCH
@SJMP 0x0049AF59, 0x0049AF99

; Acceleration
@PATCH 0x0049B052
    fstp dword [esp+0x924]
    cvttss2si ebp, dword [esp+0x924]
@ENDPATCH
@SJMP 0x0049B062, 0x0049B09D

; Speed (strict mode)
@PATCH 0x0049B14E
    fstp dword [esp+0x924]
    cvttss2si ebx, dword [esp+0x924]
@ENDPATCH
@SJMP 0x0049B15E, 0x0049B199

; Speed (non-strict mode)
@PATCH 0x0049B1C0
    fstp dword [esp+0x924]
    cvttss2si ecx, dword [esp+0x924]
@ENDPATCH
@SJMP 0x0049B1D0, 0x0049B20B
