#!/usr/bin/env bash
# RXos — automated persistence test (`make test-disk`).
#
# Proves real cross-reboot persistence end to end, on BOTH storage backends:
#   - virtio-blk on q35   (modern, v4.2)   -> device "vda"
#   - AHCI SATA on q35    (real HW, v4.8)  -> device "sda"
#   - ATA PIO on pc       (legacy, v4.1)   -> device "hda"
# For each: boot 1 formats + writes + saves + halts; boot 2 reboots the same
# image and asserts the profile auto-restores ("Welcome back"), the file
# survives, and the host-side inspector sees it.
set -u
cd "$(dirname "$0")/.."

ISO="${1:-build/rxOS-7.0.0-vm.iso}"
[[ -f "$ISO" ]] || { echo "ISO not found: $ISO  (run: make iso)"; exit 1; }

pass=0; fail=0
need() {  # need <file> <marker>
    if grep -qF "$2" "$1"; then printf '    [ok]   %s\n' "$2"; pass=$((pass+1))
    else printf '    [FAIL] missing in %s: %s\n' "$1" "$2"; fail=$((fail+1)); fi
}

# run_case <name> <dev> <img> <qemu-storage-args...>
run_case() {
    local name="$1" dev="$2" img="$3"; shift 3
    local args=("$@")
    echo "[persist] backend: $name (device $dev)"
    tools/make_disk_image.sh "$img" >/dev/null

    local in1 in2 log1 log2
    in1="$(mktemp)"; in2="$(mktemp)"
    log1="build/persist_${dev}_1.log"; log2="build/persist_${dev}_2.log"
    printf '\np_%s\npersist\npersist\nformat %s yes\nwrite keep.txt hola-%s\nsave\nhalt\n' "$dev" "$dev" "$dev" > "$in1"
    printf '\npersist\ncat keep.txt\nls\nhalt\n' > "$in2"

    boot() {  # boot <input> <log>
        timeout 90 qemu-system-x86_64 "${args[@]}" -m 512M -cdrom "$ISO" \
            -display none -vga std -serial stdio -monitor none \
            -no-reboot -no-shutdown < "$1" > "$2" 2>/dev/null &
        local pid=$!
        for _ in $(seq 1 80); do grep -q "RXos halted" "$2" 2>/dev/null && break; sleep 1; done
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    }
    boot "$in1" "$log1"
    boot "$in2" "$log2"
    rm -f "$in1" "$in2"

    need "$log1" "disk formatted (empty RXFS image)"
    need "$log1" "wrote 8 B to keep.txt"        # "hola-vda" / "hola-hda" = 8 bytes
    need "$log1" "saved 5 file(s) to $dev"
    need "$log2" "restored 5 file(s) from $dev (persistent RXFS)"
    need "$log2" "Welcome back, p_$dev"
    need "$log2" "hola-$dev"
    if python3 tools/inspect_image.py "$img" --cat keep.txt | grep -q "hola-$dev"; then
        printf '    [ok]   host inspect_image.py reads keep.txt\n'; pass=$((pass+1))
    else printf '    [FAIL] host image missing keep.txt\n'; fail=$((fail+1)); fi
}

run_case "virtio-blk / q35" vda build/persist-vda.img \
    -machine q35 -drive file=build/persist-vda.img,format=raw,if=none,id=d0 \
    -device virtio-blk-pci,drive=d0
# q35's built-in ICH9 AHCI controller is the same class 01:06:01 device a real
# laptop exposes, so this exercises the driver the HP 15-ac195nl needs.
run_case "AHCI SATA / q35"  sda build/persist-sda.img \
    -machine q35 -drive file=build/persist-sda.img,format=raw,if=none,id=d0 \
    -device ide-hd,drive=d0,bus=ide.0
run_case "ATA PIO / pc"     hda build/persist-hda.img \
    -machine pc  -drive file=build/persist-hda.img,format=raw,if=ide

echo
if [[ $fail -eq 0 ]]; then
    echo "PERSISTENCE TEST PASS ($pass checks) - files survive a reboot on all three backends"
else
    echo "PERSISTENCE TEST FAIL ($fail failed, $pass ok)"
    exit 1
fi
