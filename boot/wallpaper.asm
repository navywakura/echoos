; EchOS 1.0 Eclipse wallpaper pack — 3 originals + 9 Rogex field series.
section .rodata

global rxos_wp_count
global rxos_wp_table

align 16
gauges_jpg:
    incbin "assets/wallpaper/pack/gauges.jpg"
gauges_jpg_end:

align 16
steel_jpg:
    incbin "assets/wallpaper/pack/steel.jpg"
steel_jpg_end:

align 16
wood_jpg:
    incbin "assets/wallpaper/pack/wood.jpg"
wood_jpg_end:

align 16
mountain_jpg:
    incbin "assets/wallpaper/pack/mountain.jpg"
mountain_jpg_end:

align 16
lake_jpg:
    incbin "assets/wallpaper/pack/lake.jpg"
lake_jpg_end:

align 16
night_jpg:
    incbin "assets/wallpaper/pack/night.jpg"
night_jpg_end:

align 16
forest_jpg:
    incbin "assets/wallpaper/pack/forest.jpg"
forest_jpg_end:

align 16
sea_jpg:
    incbin "assets/wallpaper/pack/sea.jpg"
sea_jpg_end:

align 16
fog_jpg:
    incbin "assets/wallpaper/pack/fog.jpg"
fog_jpg_end:

align 16
dune_jpg:
    incbin "assets/wallpaper/pack/dune.jpg"
dune_jpg_end:

align 16
portrait_jpg:
    incbin "assets/wallpaper/pack/portrait.jpg"
portrait_jpg_end:

align 16
meadow_jpg:
    incbin "assets/wallpaper/pack/meadow.jpg"
meadow_jpg_end:

align 8
rxos_wp_count:
    dq 12

align 8
rxos_wp_table:
    dq gauges_jpg
    dq gauges_jpg_end - gauges_jpg
    dq gauges_name
    dq steel_jpg
    dq steel_jpg_end - steel_jpg
    dq steel_name
    dq wood_jpg
    dq wood_jpg_end - wood_jpg
    dq wood_name
    dq mountain_jpg
    dq mountain_jpg_end - mountain_jpg
    dq mountain_name
    dq lake_jpg
    dq lake_jpg_end - lake_jpg
    dq lake_name
    dq night_jpg
    dq night_jpg_end - night_jpg
    dq night_name
    dq forest_jpg
    dq forest_jpg_end - forest_jpg
    dq forest_name
    dq sea_jpg
    dq sea_jpg_end - sea_jpg
    dq sea_name
    dq fog_jpg
    dq fog_jpg_end - fog_jpg
    dq fog_name
    dq dune_jpg
    dq dune_jpg_end - dune_jpg
    dq dune_name
    dq portrait_jpg
    dq portrait_jpg_end - portrait_jpg
    dq portrait_name
    dq meadow_jpg
    dq meadow_jpg_end - meadow_jpg
    dq meadow_name

gauges_name: db "gauges", 0
steel_name: db "steel", 0
wood_name: db "wood", 0
mountain_name: db "mountain", 0
lake_name: db "lake", 0
night_name: db "night", 0
forest_name: db "forest", 0
sea_name: db "sea", 0
fog_name: db "fog", 0
dune_name: db "dune", 0
portrait_name: db "portrait", 0
meadow_name: db "meadow", 0
