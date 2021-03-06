#! /bin/bash

mkdir ../qemu
pushd ../qemu

qemu-img create -f raw rootfs.img 8G
parted rootfs.img --script mklabel msdos
parted rootfs.img --script mkpart primary ext4 4M 100%

sudo modprobe nbd max_part=16
sudo qemu-nbd -c /dev/nbd0 rootfs.img
sudo mkfs.ext4 -O ^huge_file /dev/nbd0p1

mkdir rootfs
sudo mount /dev/nbd0p1 rootfs
sudo debootstrap --arch=armhf --keyring=/usr/share/keyrings/ubuntu-archive-keyring.gpg --include openssh-server,gdb,strace --verbose --foreign vivid rootfs

# Provision new system
echo "proc              /proc   proc    nodev,noexec,nosuid         0 0" | sudo tee rootfs/etc/fstab
echo "tmpfs             /tmp    tmpfs   nodev,noexec,nosuid         0 0" | sudo tee -a rootfs/etc/fstab
echo "/dev/mmcblk0p1    /       ext4    relatime,errors=remount-ro  0 1" | sudo tee -a rootfs/etc/fstab
echo -e "auto eth0\niface eth0 inet dhcp" | sudo tee -a rootfs/etc/network/interfaces.d/eth0
echo "qemu" | sudo tee rootfs/etc/hostname

sudo umount rootfs
sudo qemu-nbd -d /dev/nbd0

echo "Booting into shell, once complete manually run '/debootstrap/debootstrap --second-stage' and shutdown"
read -rsp $'Press any key to continue...\n' -n1 key
qemu-system-arm -M vexpress-a9 -cpu cortex-a9 -m 1024M -kernel ../linux/arch/arm/boot/zImage -dtb ../linux/arch/arm/boot/dts/vexpress-v2p-ca9.dtb -sd rootfs.img -append "rootwait console=ttyAMA0 root=/dev/mmcblk0p1 init=/bin/sh rw" -nographic

# Allow login as root, no password set though
sudo qemu-nbd -c /dev/nbd0 rootfs.img
sudo mount /dev/nbd0p1 rootfs
sudo sed -e "s/PermitRootLogin without-password/PermitRootLogin yes/" -i rootfs/etc/ssh/sshd_config
sudo umount rootfs
sudo qemu-nbd -d /dev/nbd0
rmdir rootfs

popd

echo "All done! Run ./run-vm.sh to start, set a root password on login"
