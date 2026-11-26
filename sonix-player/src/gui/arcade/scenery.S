.section .rodata
.balign 4
.global scene_cliffs
scene_cliffs:
.incbin "assets/arcade/cliffs.rgb565"
.global scene_observatory
scene_observatory:
.incbin "assets/arcade/observatory.rgb565"
.global scene_crystal
scene_crystal:
.incbin "assets/arcade/crystal.rgb565"
.global scene_reef
scene_reef:
.incbin "assets/arcade/reef.rgb565"
.global scene_tabletop
scene_tabletop:
.incbin "assets/arcade/tabletop.rgb565"
.global scene_enamel
scene_enamel:
.incbin "assets/arcade/enamel.rgb565"
.global scene_lake
scene_lake:
.incbin "assets/arcade/lake.rgb565"
.global stone_limestone
stone_limestone:
.incbin "assets/arcade/limestone.rgb565"
.global stone_column
stone_column:
.incbin "assets/arcade/column.rgb565"
.global player_sleeve_pixels
player_sleeve_pixels:
.incbin "assets/player/sleeve.rgb565"
.section .note.GNU-stack,"",@progbits
