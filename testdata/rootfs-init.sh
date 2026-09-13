#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev 2>/dev/null || true
# Epoch de RTC: nao ha CONFIG_RTC_CLASS nem RTC modelado, entao o kernel comeca em
# 1970. O harness passa a hora do host em zeebo_epoch=<unix> na cmdline (ZEEBO_EPOCH=0
# desliga). O timer ja' anda certo; isto so' ajusta o ponto de partida.
for a in $(cat /proc/cmdline); do
	case "$a" in
	zeebo_epoch=*)
		e="${a#zeebo_epoch=}"
		[ -n "$e" ] && date -s "@$e" >/dev/null 2>&1 && echo "[init] relogio: $(date)"
		;;
	esac
done
echo "========================================"
echo "  ZEEBO LINUX CLEAN ROOTFS INITIALIZED  "
echo "========================================"
uname -a
# teclado do host (UART) -> console de VT, para o que se digita aparecer no fbcon
/vtbridge &
sleep 1
echo "Prompting shell no console de VT (tty0)..."
exec /bin/sh </dev/tty0 >/dev/tty0 2>&1
