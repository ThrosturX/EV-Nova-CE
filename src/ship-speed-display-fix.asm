%include "macros/patch.inc"

; Fix ship speed values displaying one below their nominal value
; due to floating-point quantization followed by truncation.
;
; For example, ship Speed 190 is stored internally as approximately
; 1.899999976, which becomes approximately 189.9999976 when converted
; back to display units. The existing FIST already rounds this to 190;
; skip the following truncation correction and preserve that result.

@SJMP 0x00495F62, 0x00495F79
