; Link the BIOS disk loader into the kernel so the LIVE installer can write it.
section .rodata

%ifndef HDBOOT_BIN
    %error "HDBOOT_BIN must name this build's disk loader"
%endif

global hdboot_stage1
global hdboot_stage2
global hdboot_stage1_len
global hdboot_stage2_len

hdboot_stage1:
    incbin HDBOOT_BIN, 0, 512
hdboot_stage1_len:
    dd 512

hdboot_stage2:
    incbin HDBOOT_BIN, 512
hdboot_stage2_end:
hdboot_stage2_len:
    dd hdboot_stage2_end - hdboot_stage2
