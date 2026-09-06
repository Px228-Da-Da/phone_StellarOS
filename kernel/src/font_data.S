/*
 * Шрифт в образе.
 *
 * Растровый Manrope готовит tools/fontgen при сборке — здесь он только
 * попадает в ядро одним куском. Отдельный файл, а не user.S: тот про
 * программы для EL0, а шрифт нужен самому ядру, в том числе в панике,
 * когда никаких программ уже нет.
 */
.section ".rodata.font", "a"

.align 3
.global font_blob_start
font_blob_start:
    .incbin "font/manrope.stf"
.align 3
.global font_blob_end
font_blob_end:
