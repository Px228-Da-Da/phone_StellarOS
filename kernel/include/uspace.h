#ifndef USPACE_H
#define USPACE_H
#include "types.h"

/*
 * Запуск программ в пользовательском режиме.
 *
 * Программа у нас пока не файл, а кусок байтов внутри образа ядра:
 * файловой системы нет, читать неоткуда. Это не мешает ей быть
 * настоящей программой — она исполняется в EL0, со своими страницами и
 * своим стеком, и обратиться к ядру может только системным вызовом.
 *
 * Раздельных адресных пространств ещё нет: таблица трансляции одна на
 * всех, и каждая программа получает в ней свой участок. От ядра
 * программы отгорожены по-настоящему — их страницы помечены как
 * доступные EL0, а ядерные нет, и процессор это соблюдает. А вот друг
 * от друга — пока нет: адреса соседа отображены и доступны EL0. Это
 * следующий шаг, ему нужен свой корень таблиц на каждую программу.
 */

/*
 * Запустить программу. Возвращает номер созданной задачи или
 * отрицательное значение, если не вышло.
 */
s64 uspace_spawn(const char *name, const void *image, u64 size);

/*
 * Где сейчас находится запуск программы.
 *
 * Не печать, а переменная — и в этом весь смысл. Запуск однажды замер на
 * телефоне, а печать по шагам сдвигала тайминг, и зависание переставало
 * повторяться. Запись в переменную не стоит почти ничего, а печатает её
 * пульс с другого ядра: он жив всегда, когда жива система вообще.
 *
 * 0 — никто ничего не запускает.
 */
extern volatile u32 uspace_stage;
extern const char *volatile uspace_who;

/*
 * Запустить программу из вшитых в образ — по номеру из общего с EL0
 * списка (см. IMG_* в syscall.h). Отсюда же берёт программы системный
 * вызов SYS_SPAWN: списка файлов у нас пока нет, а имя, пришедшее из
 * EL0, всё равно пришлось бы сверять с этим же списком.
 */
s64 uspace_spawn_image(u32 index);

/* Программы, вшитые в образ. Границы расставляет ассемблер, см. user.S */
extern const u8 user_hello_start[];
extern const u8 user_hello_end[];
extern const u8 user_rogue_start[];
extern const u8 user_rogue_end[];
extern const u8 user_spin_start[];
extern const u8 user_spin_end[];
extern const u8 user_twin_start[];
extern const u8 user_twin_end[];
extern const u8 user_once_start[];
extern const u8 user_once_end[];
extern const u8 user_boss_start[];
extern const u8 user_boss_end[];
extern const u8 user_paint_start[];
extern const u8 user_paint_end[];
extern const u8 user_track_start[];
extern const u8 user_track_end[];
extern const u8 user_panel_start[];
extern const u8 user_panel_end[];

#endif
