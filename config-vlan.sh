#!/bin/sh
# set -x
# Yinghao.Xing 2021-05-18 11:28
# config-vlan.sh 
# Set up VLAN with following parameters
# $1 enable vlan 0:disable vlan  1:enable vlan
# $2 WAN VLAN ID 0~4095 [note:don`t support vid 0 4095]
# $3 WAN VLAN PRIORITY
# $4 LAN VLAN ID 0~4095 [note:don`t support vid 0 4095]
# $5 LAN VLAN PRIORITY
#####################################################
#                  CPU PORT                         #
#                      |                            #
#       _ _ _ _ _ _ _ _|_ _ _ _ _ _ _ _ _           #
#      |                                 |          #
#      |                                 |          #
#   WAN PORT         SWITCH           LAN PORT      #
#      |                                 |          #
#      |                                 |          #
#   internet                             PC         #
#####################################################

if [ $# -ne 5 ]; then
    echo "Syntax : $0 <VLAN Enabel> <WAN Vlan ID> <WAN Vlan Priority> <LAN Vlan ID> <LAN Vlan Priority>"
    exit 0;
fi

VLAN_ENABLE=$1
WAN_VLAN_ID=$2
WAN_VLAN_PRI=$3
LAN_VLAN_ID=$4
LAN_VLAN_PRI=$5
DEFAULT_VLAN_ID=1

#pbmp: Port bitmap of VLAN ports.
#ubmp: Port bitmap of VLAN ports whose associated packets will not contain the IEEE 802.1q tag header.
pbmp=0x000
ubmp=0x000

EXTERNAL_SWITCH_NAME=`prop_get sys.net.external.switch`
NET_IFNAME=`prop_get sys.net.eth.ifname`
echo "use $EXTERNAL_SWITCH_NAME switch vlan port!"
#port value in external switch datasheet.
if [ $EXTERNAL_SWITCH_NAME == "rtl8304" ];then
    PORT_LAN=3
    PORT_WAN=0
    PORT_CPU=8
    PORT_LAN_BITMAP=0x008
    PORT_WAN_BITMAP=0x001
    PORT_CPU_BITMAP=0x100
elif [ $EXTERNAL_SWITCH_NAME == "rtl8363" ];then
    PORT_LAN=3
    PORT_WAN=1
    PORT_CPU=16
    PORT_LAN_BITMAP=0x08 #0x01 << $PORT_LAN
    PORT_WAN_BITMAP=0x02 #0x01 << $PORT_WAN
    PORT_CPU_BITMAP=0x10000 #0x01 << $PORT_CPU
elif [ $EXTERNAL_SWITCH_NAME == "ip175" ];then
    PORT_LAN=1
    PORT_WAN=0
    PORT_CPU=5
    PORT_LAN_BITMAP=0x02 #0x01 << $PORT_LAN
    PORT_WAN_BITMAP=0x01 #0x01 << $PORT_WAN
    PORT_CPU_BITMAP=0x20 #0x01 << $PORT_CPU
elif [ $EXTERNAL_SWITCH_NAME == "qca8334" ];then
    PORT_LAN=3
    PORT_WAN=2
    PORT_CPU=0
    PORT_LAN_BITMAP=0x08 #0x01 << $PORT_LAN
    PORT_WAN_BITMAP=0x04 #0x01 << $PORT_WAN
    PORT_CPU_BITMAP=0x01 #0x01 << $PORT_CPU
elif [ $EXTERNAL_SWITCH_NAME == "an8852" ];then
    PORT_LAN=0
    PORT_WAN=1
    PORT_CPU=5
    PORT_LAN_BITMAP=0x01 #0x01 << $PORT_LAN
    PORT_WAN_BITMAP=0x02 #0x01 << $PORT_WAN
    PORT_CPU_BITMAP=0x20 #0x01 << $PORT_CPU
else
    echo "unknown switch:$EXTERNAL_SWITCH_NAME!!!, need fill external switch in config-vlan.sh!!!"
    exit 0;
fi

# Initiallize or re-init VLAN
if [ $VLAN_ENABLE == "1" ];then
    #初始化vlan , vlan id 默认为1
    ethtool --init-vlan $NET_IFNAME
else
    if [ $WAN_VLAN_ID != "-1" ];then
        ethtool --remove-vlan-port $NET_IFNAME vid $WAN_VLAN_ID
    fi

    if [ $LAN_VLAN_ID != "-1" ];then
        ethtool --remove-vlan-port $NET_IFNAME vid $LAN_VLAN_ID
    fi

    ethtool --uninit-vlan $NET_IFNAME
    exit 0;
fi

#如果wan vlan id 为 1 时， 默认vlan id为4094
if [ $WAN_VLAN_ID == $DEFAULT_VLAN_ID -o $LAN_VLAN_ID == $DEFAULT_VLAN_ID ];then
    DEFAULT_VLAN_ID=4094

    let pbmp=$PORT_WAN_BITMAP+$PORT_LAN_BITMAP+$PORT_CPU_BITMAP
    let ubmp=$PORT_WAN_BITMAP+$PORT_LAN_BITMAP+$PORT_CPU_BITMAP

    ethtool --add-vlan-port $NET_IFNAME vid $DEFAULT_VLAN_ID mbrmsk $pbmp untagmsk $ubmp

    ethtool --set-untagged-vlan-port $NET_IFNAME port $PORT_WAN  vid $DEFAULT_VLAN_ID
    ethtool --set-untagged-vlan-port $NET_IFNAME port $PORT_LAN  vid $DEFAULT_VLAN_ID
    ethtool --set-untagged-vlan-port $NET_IFNAME port $PORT_CPU  vid $DEFAULT_VLAN_ID
fi

if [ $WAN_VLAN_ID == $LAN_VLAN_ID -a $WAN_VLAN_ID != "-1" ];then
    let pbmp=$PORT_CPU_BITMAP+$PORT_WAN_BITMAP+$PORT_LAN_BITMAP
    let ubmp=$PORT_CPU_BITMAP+$PORT_LAN_BITMAP

    ethtool --add-vlan-port $NET_IFNAME vid $WAN_VLAN_ID mbrmsk $pbmp untagmsk $ubmp

    ethtool --set-untagged-vlan-port $NET_IFNAME port $PORT_LAN vid $WAN_VLAN_ID
    ethtool --set-untagged-vlan-port $NET_IFNAME port $PORT_WAN vid $WAN_VLAN_ID
    ethtool --set-untagged-vlan-port $NET_IFNAME port $PORT_CPU vid $WAN_VLAN_ID
 
    ethtool --set-untagged-vlan-port-priority $NET_IFNAME port $PORT_CPU  priority $WAN_VLAN_PRI
    ethtool --set-untagged-vlan-port-priority $NET_IFNAME port $PORT_WAN  priority $WAN_VLAN_PRI
    ethtool --set-untagged-vlan-port-priority $NET_IFNAME port $PORT_LAN  priority $WAN_VLAN_PRI
    exit 0;
fi

if [ $WAN_VLAN_ID != "-1" ];then
    let pbmp=$PORT_CPU_BITMAP+$PORT_WAN_BITMAP
    let ubmp=$PORT_CPU_BITMAP
    ethtool --add-vlan-port $NET_IFNAME vid $WAN_VLAN_ID mbrmsk $pbmp untagmsk $ubmp

    ethtool --set-untagged-vlan-port $NET_IFNAME port $PORT_CPU vid $WAN_VLAN_ID
    ethtool --set-untagged-vlan-port-priority $NET_IFNAME port $PORT_CPU  priority $WAN_VLAN_PRI

    let pbmp=$PORT_WAN_BITMAP+$PORT_LAN_BITMAP
    let ubmp=$PORT_WAN_BITMAP+$PORT_LAN_BITMAP
    ethtool --add-vlan-port $NET_IFNAME vid $DEFAULT_VLAN_ID mbrmsk $pbmp untagmsk $ubmp
    ethtool --set-untagged-vlan-port $NET_IFNAME port $PORT_WAN  vid $DEFAULT_VLAN_ID
fi

if [ $LAN_VLAN_ID != "-1" ];then
    let pbmp=$PORT_LAN_BITMAP+$PORT_WAN_BITMAP
    let ubmp=$PORT_LAN_BITMAP
    ethtool --add-vlan-port $NET_IFNAME vid $LAN_VLAN_ID mbrmsk $pbmp untagmsk $ubmp
    ethtool --set-untagged-vlan-port $NET_IFNAME port $PORT_LAN  vid $LAN_VLAN_ID
    ethtool --set-untagged-vlan-port-priority $NET_IFNAME port $PORT_LAN  priority $LAN_VLAN_PRI
fi
