#!/usr/bin/env bash
# RXos — two-node Rogex Wired ping test (robust).
# Boots two QEMU nodes on a shared multicast wire; node B pings node A by
# pseudonym and must get a PONG (A's alias shows in B's Wired panel).
# NIC=<qemu device> selects the NIC model:
#   virtio-net-pci (default, QEMU-native) | e1000 (VirtualBox's 82540EM)
set -u
cd "$(dirname "$0")/.."
# shellcheck source=qemu_host.sh
. tools/qemu_host.sh
ISO="${1:-build/rxOS-7.0.0-vm.iso}"
NIC="${NIC:-virtio-net-pci}"

MCAST=230.0.0.7:11299
pkill -9 -f "rxOS-7.0.0" 2>/dev/null; sleep 1
rm -f build/a.log build/b.log build/mon_a.sock build/mon_b.sock build/bob_net.*

"$QEMU" $QEMU_HOST_ARGS -machine q35 -m 512M -cdrom "$ISO" \
  -netdev socket,id=n0,mcast=$MCAST -device $NIC,netdev=n0,mac=52:54:00:aa:aa:aa \
  -serial file:build/a.log -monitor unix:build/mon_a.sock,server,nowait \
  -display none -vga std -no-reboot -no-shutdown & APID=$!
"$QEMU" $QEMU_HOST_ARGS -machine q35 -m 512M -cdrom "$ISO" \
  -netdev socket,id=n0,mcast=$MCAST -device $NIC,netdev=n0,mac=52:54:00:bb:bb:bb \
  -serial file:build/b.log -monitor unix:build/mon_b.sock,server,nowait \
  -display none -vga std -no-reboot -no-shutdown & BPID=$!

ma(){ printf '%s\n' "$1" | nc -q1 -U build/mon_a.sock >/dev/null 2>&1; }
mb(){ printf '%s\n' "$1" | nc -q1 -U build/mon_b.sock >/dev/null 2>&1; }
ka(){ ma "sendkey $1"; sleep 0.14; }
kb(){ mb "sendkey $1"; sleep 0.14; }
type_word(){ local fn=$1; shift; for k in "$@"; do $fn "$k"; done; }

# wait until BOTH nodes reach the initial registration username prompt
for i in $(seq 1 40); do
  if grep -qE "username|pseudonym>" build/a.log 2>/dev/null && \
     grep -qE "username|pseudonym>" build/b.log 2>/dev/null; then break; fi
  sleep 1
done
echo "both at registration after ${i}s"

# set usernames + matching passwords (password + confirm)
type_word ka a l i c e; ka ret
type_word ka a l i c e 1; ka ret
type_word ka a l i c e 1; ka ret
type_word kb b o b;     kb ret
type_word kb b o b 1;   kb ret
type_word kb b o b 1;   kb ret
# wait until both unlocked / welcomed
for i in $(seq 1 30); do
  if grep -q "Welcome" build/a.log 2>/dev/null && grep -q "Welcome" build/b.log 2>/dev/null; then break; fi
  sleep 1
done
echo "both on desktop after ${i}s"
sleep 2

# Boot now lands on the graphical desktop, where a keystroke is a hotkey and
# not a command. Esc drops to the classic shell, whose command bar mirrors its
# output to the serial console — which is what this test reads.
kb esc; sleep 2

# bob pings alice
type_word kb p i n g spc a l i c e; kb ret
sleep 4
mb "screendump build/bob_net.ppm"; sleep 1
pnmtopng build/bob_net.ppm > build/bob_net.png 2>/dev/null

echo "=== bob Wired panel (serial mirror) ==="
grep -iE "Alias:|MAC:|Link:|Peers who|ping complete|no reply|\* " build/b.log | tail -14
kill $APID $BPID 2>/dev/null
echo "NETTEST_DONE"
