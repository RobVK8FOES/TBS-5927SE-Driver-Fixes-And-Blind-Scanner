# TBS-5927SE-Driver-Fixes-And-Blind-Scanner
Patches for TBS driver version 1016 to enable blind scanning and generic stream data capture on the new 5927SE.

Tested and working on DragonOS FocalX R37.1, built upon Lubuntu 22.04.5 LTS. Kernel version 6.8.0-45-generic.

Vibe-coded with Google Gemini Neon on 'high' pre-set. USE AT YOUR OWN RISK! 

## Driver Download, Patch, And Install Procedure

NOTE: A lot of the following commands are very long, ensure that you scroll to the right ---->
```bash
sudo apt-get update

sudo apt-get install gcc-12 -y

wget --no-check-certificate -U "Mozilla/5.0" "[https://www.tbsiptv.com/download/common/tbsdvb_v1016.tar.bz2](https://www.tbsiptv.com/download/common/tbsdvb_v1016.tar.bz2)"

tar -xvjf tbsdvb_v1016.tar.bz2

cd tbsdvb

wget --no-check-certificate -U "Mozilla/5.0" "https://raw.githubusercontent.com/RobVK8FOES/TBS-5927SE-Driver-Fixes-And-Blind-Scanner/refs/heads/master/tbs5927se_driver_fixes.patch"

patch -p1 < tbs5927se_driver_fixes.patch

sudo ./install.sh

sudo depmod -a
```

It is recommended to reboot before proceeding.

## Blind Scanner Application Build And Usage

```bash
wget --no-check-certificate -U "Mozilla/5.0" "https://raw.githubusercontent.com/RobVK8FOES/TBS-5927SE-Driver-Fixes-And-Blind-Scanner/refs/heads/master/tbs5927se_blindscan.c"

gcc -O2 -Wall -o tbs5927se_blindscan tbs5927se_blindscan.c -lm
```

Run the binary with the -h flag to see configurable options:

```bash
./tbs5927se_blindscan -h
```

Optional install and clean up:

```bash
sudo cp -v tbs5927se_blindscan /usr/local/bin/tbs5927se_blindscan

rm -v tbs5927se_blindscan.c tbs5927se_blindscan
```

Blind scanner example usage = Scan Ku-band 10700 - 12750 MHz, H and V polarity with a universal LNB, and output found transponders to a dvbv5-zap compatible channels.conf file:

```bash
./tbs5927se_blindscan -b full -p vh -d 1 -n 200 -o channels.conf
```

## Switching Driver To Generic Stream Capture Mode:

NOTE: This will probably prevent your 5927SE from watching television, listening to radio, or receiving MPE/Multicast transponders!
```bash
sudo modprobe -r dvb_usb_tbs5927se avl62x1

sudo modprobe avl62x1 stream=1
   
sudo modprobe dvb_usb_tbs5927se raw_mode=1
```

Confirm if the driver was successsfully switched:
```bash
cat /sys/module/avl62x1/parameters/stream /sys/module/dvb_usb_tbs5927se/parameters/raw_mode
```
Two lines with the number 1 printed on them means the switch was successful.
