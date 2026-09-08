#!/system/bin/sh

TAG="bt_loader"

bkbIsBroken=0
#for Android
Serialno=$(getprop ro.serialno)
#for big Linux systems
#Serialno=$(echo $(dmesg | grep -i androidboot.serialno) | sed 's|.*serialno=||' | awk '{print $1}')
md5serialno=$(echo -n $Serialno | md5sum)

checkBkbPartition() {
	echo "$TAG: attempt to read BKB partition"
	#local BKB=$(cat /dev/block/platform/sdhci-tegra.3/by-name/BKB | grep -a XIAOMI)
	#if [ "$BKB" = "" ]; then
	bkbIsBroken=1
	#fi;
}

generateBtMac() {
	echo "$TAG: generating bt mac address"
	local btMac="${md5serialno:3:2}:${md5serialno:16:2}:${md5serialno:17:2}:${md5serialno:9:2}:${md5serialno:11:2}:${md5serialno:13:2}"
	mkdir -p /data/misc/bluetooth
	echo $btMac > /data/misc/bluetooth/mocha_btmacaddr.txt
	chmod 660 /data/misc/bluetooth/mocha_btmacaddr.txt
	chown bluetooth:bluetooth /data/misc/bluetooth/mocha_btmacaddr.txt
	setprop ro.bt.bdaddr_path /data/misc/bluetooth/mocha_btmacaddr.txt
	setprop persist.service.bdroid.bdaddr $btMac
	setprop ro.boot.btmacaddr $btMac
}

generateWifiMac() {
	echo "$TAG: generating wifi mac address"
	local wifiMac="0c:1d:${md5serialno:7:2}:${md5serialno:9:2}:${md5serialno:11:2}:${md5serialno:14:2}"
	mkdir -p /data/misc/wifi
	echo $wifiMac > /data/misc/wifi/mocha_macaddr.txt
	chmod 660 /data/misc/wifi/mocha_macaddr.txt
	chown wifi:wifi /data/misc/wifi/mocha_macaddr.txt
}

main() {
	checkBkbPartition

	if [ "$bkbIsBroken" = 1 ]; then
		generateBtMac
		generateWifiMac
	else
		echo "$TAG: BKB partion is not broken"
	fi
}

main
