/* vtbridge - ponte teclado (serial) -> console de VT (fbcon).
 *
 * O harness injeta as teclas na UART do guest (ttyMSM2). Para a digitacao aparecer
 * no framebuffer, este processo le' os bytes da serial e os injeta na fila de
 * entrada do /dev/tty0 com TIOCSTI; o shell roda com stdin/stdout no tty0, entao o
 * eco da digitacao e as respostas saem pelo console de VT -- que e' justamente o que
 * o fbcon desenha na memoria de framebuffer.
 *
 * A serial entra em modo raw (sem ICANON/ECHO) para o byte chegar na hora e nao
 * haver eco duplicado: quem ecoa e' o console de VT.
 *
 * Compilado estatico para o rootfs:
 *   arm-none-linux-gnueabi-gcc -static -O2 -o /work/rootfs/vtbridge vtbridge.c
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int main(void) {
    int in = open("/dev/ttyMSM2", O_RDONLY | O_NONBLOCK);
    if (in < 0)
        in = open("/dev/console", O_RDONLY | O_NONBLOCK);
    int vt = open("/dev/tty0", O_RDWR);
    if (in < 0 || vt < 0) {
        perror("vtbridge");
        return 1;
    }
    struct termios t;
    if (tcgetattr(in, &t) == 0) {
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(in, TCSANOW, &t);
    }
    fprintf(stderr, "vtbridge: pronto (serial -> tty0)\n");
    char c;
    for (;;) {
        ssize_t n = read(in, &c, 1);
        if (n == 1) {
            if (ioctl(vt, TIOCSTI, &c) != 0)
                fprintf(stderr, "vtbridge: TIOCSTI falhou\n");
        } else {
            usleep(20000);
        }
    }
    return 0;
}
