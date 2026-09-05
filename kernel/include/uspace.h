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

/* Запустить программу. Возвращает 0, если задача создана. */
int uspace_spawn(const char *name, const void *image, u64 size);

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

#endif
