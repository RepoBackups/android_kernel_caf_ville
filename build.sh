CROSS_COMPILE=/home/andrew/htc/kernels/toolchains/prebuilts_gcc_linux-x86_arm_arm-eabi-4.9-linaro-a15/bin/arm-eabi-
#INITRAMFS_DIR=ramdisk.gz
#KERNEL_NAME=IceCode1.0

# DO NOT MODIFY BELOW THIS LINE
CURRENT_DIR=`pwd`
NB_CPU=`grep processor /proc/cpuinfo | wc -l`
let NB_CPU+=1
if [[ -z $1 ]]
then
	echo "No configuration file defined"
	exit 1

else 
	if [[ ! -e "${CURRENT_DIR}/arch/arm/configs/$1" ]]
	then
		echo "Configuration file $1 not found"
		exit 1
	fi
	CONFIG=$1
fi

make $1
echo "Building kernel ${KBUILD_BUILD_VERSION} with configuration $CONFIG"
make ARCH=arm -j$NB_CPU CROSS_COMPILE=$CROSS_COMPILE

cp arch/arm/boot/zImage /home/andrew/htc/kernels/ModBubba/
find . -name \*.ko -exec cp '{}' /home/andrew/htc/kernels/ModBubba/modules/ ';'

echo "Done."
