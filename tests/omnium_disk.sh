#!/bin/bash
# OFS3 на диске (omnium/ofs_disk_test.tb): несколько запусков ядра на одном образе диска.
# Между запусками портим диск, как это делает жизнь: сбой при записи суперблока, испорченный сектор.
# Вывод всех запусков сравнивается с omnium/ofs_disk_test.out
cd "$(dirname "$0")/.." || exit 1
work=$(mktemp -d)
img="$work/disk.img"
./build/tb bare omnium/ofs_disk_test.tb > "$work/build.txt" 2>&1 || { cat "$work/build.txt"; exit 1; }
python3 -c "open('$img','wb').truncate(8 << 20)"
run() {
    echo "--- $1"
    perl -e 'alarm shift; exec @ARGV' 60 qemu-system-x86_64 -no-reboot -kernel omnium/ofs_disk_test.elf -serial stdio -display none \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 -drive file="$img",format=raw,if=ide,index=0 < /dev/null 2> /dev/null | tr -d '\r'
}
{
run "чистый диск"
run "второй запуск"
# сбой: свежий суперблок не дописан (сектор с бо́льшим поколением — нули) — диск берёт прошлый
python3 - "$img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
g = []
for n in (0, 1):
    f.seek(n * 512); b = f.read(512)
    g.append(struct.unpack('<q', b[8:16])[0] if b[:4] == b'OFS3' else -1)
f.seek((0 if g[0] > g[1] else 1) * 512); f.write(bytes(512))
PY
run "сбой при записи суперблока: последняя запись пропала, остальное цело"
# испорченный сектор данных у файла «большой»: чтение говорит «повреждён», а не отдаёт мусор
python3 - "$img" <<'PY'
import sys
f = open(sys.argv[1], 'r+b'); d = f.read()
at = d.find('большой'.encode())
hdr = at - 48
f.seek(hdr + 512 + 100); f.write(b'#')
PY
run "испорченный сектор"
# тот же OFS3 на SATA (AHCI) и на NVMe: два запуска — второй видит, что записал первый
for kind in ahci nvme; do
    python3 -c "open('$work/$kind.img','wb').truncate(8 << 20)"
    for n in 1 2; do
        echo "--- $kind, запуск $n"
        if [ $kind = ahci ]; then dev="-device ahci,id=a0 -drive id=s0,file=$work/$kind.img,format=raw,if=none -device ide-hd,drive=s0,bus=a0.0"
        else dev="-drive id=n0,file=$work/$kind.img,format=raw,if=none -device nvme,serial=om1,drive=n0"; fi
        perl -e 'alarm shift; exec @ARGV' 60 qemu-system-x86_64 -no-reboot -kernel omnium/ofs_disk_test.elf -serial stdio -display none \
            -device isa-debug-exit,iobase=0xf4,iosize=0x04 $dev < /dev/null 2> /dev/null | tr -d '\r' | head -3
    done
done
# флешку с exFAT OFS3 не размечает: чужая файловая система
python3 -c "import gzip,shutil; shutil.copyfileobj(gzip.open('tests/omnium/flash.img.gz'), open('$work/flash.img','wb'))"
echo "--- флешка вместо диска"
perl -e 'alarm shift; exec @ARGV' 60 qemu-system-x86_64 -no-reboot -kernel omnium/ofs_disk_test.elf -serial stdio -display none \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 -drive file="$work/flash.img",format=raw,if=ide,index=0 < /dev/null 2> /dev/null | tr -d '\r' | head -1
# все три вида дисков сразу: IDE, SATA, NVMe — номера по порядку, сектор пишется и читается
./build/tb bare omnium/disks_test.tb > "$work/build.txt" 2>&1 || { cat "$work/build.txt"; exit 1; }
echo "--- IDE + SATA + NVMe"
perl -e 'alarm shift; exec @ARGV' 60 qemu-system-x86_64 -no-reboot -kernel omnium/disks_test.elf -serial stdio -display none \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 -drive file="$work/ahci.img",format=raw,if=ide,index=0 \
    -device ahci,id=a0 -drive id=s0,file=$work/nvme.img,format=raw,if=none -device ide-hd,drive=s0,bus=a0.0 \
    -drive id=n0,file=$work/disk.img,format=raw,if=none -device nvme,serial=om1,drive=n0 < /dev/null 2> /dev/null | tr -d '\r'
} > "$work/got.txt"
# exFAT: флешка из mkfs.exfat с файлами, записанными Linux (tests/omnium/flash.img.gz)
./build/tb bare omnium/exfat_test.tb > "$work/build.txt" 2>&1 || { cat "$work/build.txt"; exit 1; }
python3 -c "import gzip,shutil; shutil.copyfileobj(gzip.open('tests/omnium/flash.img.gz'), open('$work/flash.img','wb'))"
{
echo "--- exFAT"
perl -e 'alarm shift; exec @ARGV' 60 qemu-system-x86_64 -no-reboot -kernel omnium/exfat_test.elf -serial stdio -display none \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 -drive file="$work/flash.img",format=raw,if=ide,index=0 < /dev/null 2> /dev/null | tr -d '\r'
} >> "$work/got.txt"
# если есть fsck.exfat (exfatprogs), он проверяет, что после записи Омниума флешка цела
if command -v fsck.exfat > /dev/null && ! fsck.exfat -n "$work/flash.img" > "$work/fsck.txt" 2>&1; then
    echo "fsck.exfat: флешка испорчена"; cat "$work/fsck.txt"; rm -rf "$work"; exit 1
fi
rm -f omnium/exfat_test.elf omnium/exfat_test.c
rm -f omnium/ofs_disk_test.elf omnium/ofs_disk_test.c omnium/disks_test.elf omnium/disks_test.c
if [ "$1" = "--update" ]; then cp "$work/got.txt" omnium/ofs_disk_test.out; fi
if cmp -s "$work/got.txt" omnium/ofs_disk_test.out; then echo "диск: прошло"; rm -rf "$work"; exit 0; fi
echo "диск: не прошло"; diff "$work/got.txt" omnium/ofs_disk_test.out | head -20; rm -rf "$work"; exit 1
