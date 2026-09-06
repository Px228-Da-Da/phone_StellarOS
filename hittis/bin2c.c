/*
 * bin2c — превратить готовое приложение .slt в кусок исходника на Си.
 *
 * Нужен, пока приложения ещё не читаются с флешки: единственный способ
 * доставить .slt в телефон — вшить его в образ ядра. Байты приложения
 * становятся массивом, массив попадает в программу-исполнитель, а та
 * запускает его как обычный файл.
 *
 * Отдельный крошечный инструмент, а не строка в Makefile с od или xxd:
 * их поведение разнится от системы к системе, а сборка не должна
 * зависеть от того, чей od установлен.
 */
#include <stdio.h>

int main(int argc, char **argv)
{
    FILE *in;
    int c, n = 0;

    if (argc != 3) {
        fprintf(stderr, "как пользоваться: bin2c приложение.slt имя_массива\n");
        return 1;
    }
    in = fopen(argv[1], "rb");
    if (!in) {
        fprintf(stderr, "bin2c: не открыть %s\n", argv[1]);
        return 1;
    }

    printf("/* Собрано из %s — правит его компилятор hittis, не человек */\n",
           argv[1]);
    printf("static const unsigned char %s[] = {\n", argv[2]);
    while ((c = fgetc(in)) != EOF) {
        printf("%s0x%02x,", (n % 12) ? " " : "    ", c);
        if (++n % 12 == 0)
            printf("\n");
    }
    if (n % 12)
        printf("\n");
    printf("};\n");
    printf("static const unsigned int %s_len = %d;\n", argv[2], n);
    fclose(in);
    return 0;
}
