#!/bin/sh
# Read-only preflight for HiBy's local recovery format. Also used when packing.
set -eu
export LC_ALL=C
model=$1
source_path=$2
mode=${3:-device}
case "$model" in r1|r3proii) ;; *) exit 1 ;; esac

work=$(mktemp -d /tmp/sonix-upt.XXXXXX)
archive_mounted=0
root_mounted=0
joined=
cleanup() {
    [ "$root_mounted" = 0 ] || umount "$work/root" || return 1
    [ "$archive_mounted" = 0 ] || umount "$work/iso" || return 1
    [ -z "$joined" ] || rm -f "$joined"
    rm -rf "$work"
}
trap cleanup EXIT
mkdir "$work/iso" "$work/root"
plain() { [ -f "$1" ] && [ ! -L "$1" ]; }
# BusyBox and GNU use md5sum. macOS provides md5 for host-side checks.
if command -v md5sum >/dev/null 2>&1; then
    digest() { md5sum | cut -d ' ' -f 1; }
else
    digest() { md5 -q; }
fi
small() { plain "$1" && [ "$(wc -c < "$1")" -le "$2" ]; }

# The host packer supplies a directory extracted from the completed ISO.
case "$mode" in
    --tree) iso=$source_path ;;
    --host)
        plain "$source_path" && [ -s "$source_path" ] || exit 1
        extractor=$(command -v 7z) || exit 1
        "$extractor" x -tiso "$source_path" -o"$work/iso" -y >/dev/null || exit 1
        iso=$work/iso ;;
    device)
        plain "$source_path" && [ -s "$source_path" ] || exit 1
        mount -t iso9660 -o loop,ro,nosuid,nodev,noexec "$source_path" "$work/iso"
        archive_mounted=1
        iso=$work/iso ;;
    *) exit 1 ;;
esac

dir=$iso/ota_v0
[ -d "$dir" ] && [ ! -L "$dir" ] || exit 1
small "$iso/ota_config.in" 128 && small "$dir/ota_update.in" 4096 && plain "$dir/ota_v0.ok" || exit 1
[ "$(tr -d '\r' < "$iso/ota_config.in")" = current_version=0 ] || exit 1
# Validate exactly the two payloads recovery will use. Never source a manifest.
awk '
    { sub(/\r$/, "") }
    /^$/ { next }
    /^ota_version=0$/ && !version && !n { version=1; next }
    /^img_type=/ && version && !field { type=substr($0,10); field=1; next }
    /^img_name=/ && field==1 { name=substr($0,10); field=2; next }
    /^img_size=[1-9][0-9]*$/ && field==2 { size=substr($0,10); field=3; next }
    /^img_md5=[0-9a-f]+$/ && field==3 {
        hash=substr($0,9)
        if (length(hash)!=32 || length(size)>9 || seen[name]++ ||
            !((type=="kernel" && name=="xImage") || (type=="rootfs" && name=="rootfs.squashfs"))) exit 1
        print name, size, hash
        n++; field=0; next
    }
    { exit 1 }
    END { if (!version || field || n!=2) exit 1 }
' "$dir/ota_update.in" > "$work/plan"

kernel=
while read -r name size hash; do
    case "$model:$name" in
        r1:xImage) [ "$size" -le 5242880 ] || exit 1 ;;
        r1:rootfs.squashfs) [ "$size" -le 47185920 ] || exit 1 ;;
    esac
    [ "$name" != xImage ] || kernel=$hash
    manifest=$dir/ota_md5_$name.$hash
    small "$manifest" 65536 || exit 1
    tr -d '\r' < "$manifest" > "$work/sums"
    awk 'length($0)!=32 || /[^0-9a-f]/ {exit 1}' "$work/sums" || exit 1
    set -- "$dir/$name".*
    count=0
    total=0
    previous=$hash
    for chunk do
        expected=$(printf '%s.%04d.%s' "$dir/$name" "$count" "$previous")
        [ "$chunk" = "$expected" ] && plain "$chunk" || exit 1
        IFS= read -r sum || exit 1
        bytes=$(wc -c < "$chunk")
        [ "$bytes" -gt 0 ] || exit 1
        actual=$(digest < "$chunk")
        [ "$actual" = "$sum" ] || exit 1
        total=$((total + bytes))
        count=$((count + 1))
        previous=$sum
    done < "$work/sums"
    [ "$count" -eq "$(wc -l < "$work/sums")" ] && [ "$total" -eq "$size" ] || exit 1
    actual=$(cat "$@" | digest)
    [ "$actual" = "$hash" ] || exit 1
done < "$work/plan"

# Recognize packages with a board declaration, plus older packages that predate it.
case "$model:$kernel" in
    r3proii:b48580b0fb42504b2871414a54d0c068|r1:44315fc82b20e617664b48c5cba6ca74) exit 1 ;;
esac
marker=$dir/ota_compatibility.in
if [ -e "$marker" ] || [ -L "$marker" ]; then
    small "$marker" 256 || exit 1
    marker=$(tr -d '\r' < "$marker")
    case "$model:$marker" in
        r1:compatibility=hiby_r1|r1:compatibility=hiby_r1_*|r3proii:compatibility=hiby_r3proii|r3proii:compatibility=hiby_r3proii_*) ;;
        *) exit 1 ;;
    esac
else
    case "$model:$kernel" in
        r1:b48580b0fb42504b2871414a54d0c068|r3proii:44315fc82b20e617664b48c5cba6ca74) ;;
        *)
            # Stock legacy images identify the board in their sound-card module.
            # The card has room for the temporary joined filesystem or verification fails.
            module="module_driver/x1600_hiby_${model}_sound_card.ko"
            if [ "$mode" = device ]; then
                joined=$(mktemp "$(dirname "$source_path")/.sonix-rootfs.XXXXXX")
                cat "$dir"/rootfs.squashfs.* > "$joined"
                mount -t squashfs -o loop,ro,nosuid,nodev,noexec "$joined" "$work/root"
                root_mounted=1
                plain "$work/root/$module" || exit 1
            else
                cat "$dir"/rootfs.squashfs.* > "$work/root.sq"
                unsquashfs -cat "$work/root.sq" "$module" >/dev/null || exit 1
            fi
            ;;
    esac
fi
# The v1.3.1 R1 kernel is the Broadcom build. HiBy's compatibility marker
# alone covers both wireless variants. Honor positive detection on the device.
if [ "$mode" = device ] && [ "$kernel" = b48580b0fb42504b2871414a54d0c068 ]; then
    [ ! -d /sys/module/aic8800_bsp ] || exit 1
    [ ! -f /run/r1_wireless_type ] || [ "$(cat /run/r1_wireless_type)" != aic ] || exit 1
fi
