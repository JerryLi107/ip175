#!/bin/sh
#set -x
# Yinghao.Xing 2021-03-23 14:35
# config-vlan.sh 
# Set up VLAN with following parameters
# $1 enable voice VLAN 1:enable 0:disable
# $2 voice VLAN ID 0~4095
# $3 data VLAN type 0: follow (VID same as voice VLAN) 1: un-tagged 2: tagged
# $4 data VLAN ID 0~4095

if [ $# -ne 6 ]; then
    echo "Syntax : $0 <Voice Vlan type> <Voice Vlan ID> <Data Vlan type> <Data Vlan ID> <Data pri> <Voice pri>"
    exit 1
fi

VOICE_VLAN_TYPE=$1
VOICE_VLAN_ID=$2
DATA_VLAN_TYPE=$3
DATA_VLAN_ID=$4
DATA_VLAN_PRI=$5
VOICE_VLAN_PRI=$6

#pbmp: Port bitmap of VLAN ports.
#ubmp: Port bitmap of VLAN ports whose associated packets will not contain the IEEE 802.1q tag header.
pbmp=0x000
ubmp=0x000
port_bit_map0=0x000
port_untag_map0=0x000
port_untag_map1=0x000

#port_bit_map:     Port bitmap of VLAN ports.
#port_add_tag_map: Port Y adds a VLAN tag defined in VLAN_TAG_Y to each outgoing packet associated with the VID_x. 
# spec_add_tag   : 0
port_bit_map1=0x000
port_add_tag_map1=0x000
port_bit_map2=0x000
port_add_tag_map2=0x000
spec_add_tag=0x000

add_vlan_port_eth0()
{
    ethtool --add-vlan-port eth0 vid $1 mbrmsk $2 untagmsk $3
}

set_untagged_vlan_port_eth0()
{
    ethtool --set-untagged-vlan-port eth0 port $1 vid $2
}

EXTERNAL_SWITCH_NAME=`prop_get sys.net.external.switch`
echo "use $EXTERNAL_SWITCH_NAME switch vlan port!"
#port value in external switch datasheet.
if [ $EXTERNAL_SWITCH_NAME == "rtl8304" ];then
    PORT_PC=3
    PORT_LAN=0
    PORT_CPU=8
    PORT_PC_BITMAP=0x008
    PORT_LAN_BITMAP=0x001
    PORT_CPU_BITMAP=0x100
elif [ $EXTERNAL_SWITCH_NAME == "rtl8363" ];then
    PORT_PC=3
    PORT_LAN=1
    PORT_CPU=16
    PORT_PC_BITMAP=0x08
    PORT_LAN_BITMAP=0x02
    PORT_CPU_BITMAP=0x10000
elif [ $EXTERNAL_SWITCH_NAME == "ip175" ];then
    PORT_PC=1
    PORT_LAN=0
    PORT_CPU=5
    PORT_PC_BITMAP=0x02
    PORT_LAN_BITMAP=0x01
    PORT_CPU_BITMAP=0x20
else
    echo "unknown switch:$PHY_NAME!!!, need fill external switch in config-vlan.sh!!!"
    exit;
fi

# Initiallize or re-init VLAN
ethtool --init-vlan eth0

#according to VLAN type their should be 2x3=6 cases of VLAN
#voice vlan type=0 and data vlan type != 2, no process need then

#create vlan table
#add vlan map

#case 0: voice_vlan_type=0 data_valn_type=0
if [ $VOICE_VLAN_TYPE -eq 0 -a $DATA_VLAN_TYPE -eq 0 ];then
  exit 0
fi

#case 1: voice_vlan_type=0 data_valn_type=1
if [ $VOICE_VLAN_TYPE -eq 0 -a $DATA_VLAN_TYPE -eq 1 ];then
    #only rtl8304
    if [ $EXTERNAL_SWITCH_NAME == "rtl8304" ] && [ $VOICE_VLAN_ID -eq "1" ];then
        echo "rtl8304 default vlan id is 1, not removed, uninit and exit"
        ethtool --uninit-vlan eth0
        exit 0
    fi
    if [ $EXTERNAL_SWITCH_NAME == "ip175" ];then
        ethtool --uninit-vlan eth0
        exit 0
    fi

    ethtool --remove-vlan-port eth0 vid $VOICE_VLAN_ID

    if [ $DATA_VLAN_ID -gt 0 ]; then
        ethtool --remove-vlan-port eth0 vid $DATA_VLAN_ID
    fi

    ethtool --uninit-vlan eth0
    exit 0
fi

#case 2: voice_vlan_type=0 data_valn_type=2
if [ $VOICE_VLAN_TYPE -eq 0 -a $DATA_VLAN_TYPE -eq 2 ];then
    
    #only rtl8304
    if [ $EXTERNAL_SWITCH_NAME == "rtl8304" ] && [ $VOICE_VLAN_ID -eq "1" ];then
        let VOICE_VLAN_ID=$VOICE_VLAN_ID+1
    elif [ $DATA_VLAN_ID -eq "1" ];then
        let VOICE_VLAN_ID=0
    else
        let VOICE_VLAN_ID=1
    fi

    let pbmp=$PORT_CPU_BITMAP+$PORT_LAN_BITMAP
    let ubmp=$PORT_CPU_BITMAP+$PORT_LAN_BITMAP
    let port_bit_map0=$PORT_PC_BITMAP+$PORT_LAN_BITMAP
    let port_untag_map0=$PORT_PC_BITMAP
    let port_add_tag_map2=$PORT_LAN_BITMAP
    # wan vlan disable, lan vlan enable
    if [ $EXTERNAL_SWITCH_NAME == "ip175" ];then
        add_vlan_port_eth0 $VOICE_VLAN_ID $pbmp $spec_add_tag
        add_vlan_port_eth0 $DATA_VLAN_ID $port_bit_map0 $port_add_tag_map2
        set_untagged_vlan_port_eth0 $PORT_CPU $VOICE_VLAN_ID
        set_untagged_vlan_port_eth0 $PORT_PC $DATA_VLAN_ID
    else
        add_vlan_port_eth0 $VOICE_VLAN_ID $pbmp $ubmp
        add_vlan_port_eth0 $DATA_VLAN_ID $port_bit_map0 $port_untag_map0
        set_untagged_vlan_port_eth0 $PORT_PC $DATA_VLAN_ID
        set_untagged_vlan_port_eth0 $PORT_LAN $VOICE_VLAN_ID
        set_untagged_vlan_port_eth0 $PORT_CPU $VOICE_VLAN_ID
    fi

fi

#case 3: voice_vlan_type=1 data_valn_type=0
if [ $VOICE_VLAN_TYPE -eq 1 -a $DATA_VLAN_TYPE -eq 0 ];then

    let pbmp=$PORT_CPU_BITMAP+$PORT_LAN_BITMAP+$PORT_PC_BITMAP
    let ubmp=$PORT_CPU_BITMAP+$PORT_PC_BITMAP
    let port_add_tag_map1=$PORT_LAN_BITMAP
    # wan vlan-id = lan vlan-id
    if [ $EXTERNAL_SWITCH_NAME == "ip175" ];then
        add_vlan_port_eth0 $VOICE_VLAN_ID $pbmp $port_add_tag_map1;
        set_untagged_vlan_port_eth0 $PORT_PC $VOICE_VLAN_ID
        set_untagged_vlan_port_eth0 $PORT_CPU $VOICE_VLAN_ID
    else
        add_vlan_port_eth0 $VOICE_VLAN_ID $pbmp $ubmp
        set_untagged_vlan_port_eth0 $PORT_PC $VOICE_VLAN_ID
        set_untagged_vlan_port_eth0 $PORT_LAN $VOICE_VLAN_ID
        set_untagged_vlan_port_eth0 $PORT_CPU $VOICE_VLAN_ID
    fi

fi

#case 4: voice_vlan_type=1 data_valn_type=1
if [ $VOICE_VLAN_TYPE -eq 1 -a $DATA_VLAN_TYPE -eq 1 ];then
    #only rtl8304
    if [ $EXTERNAL_SWITCH_NAME == "rtl8304" ] && [ $VOICE_VLAN_TYPE -eq "1" ] && [ $VOICE_VLAN_ID -eq "1" ] ;then
        let DATA_VLAN_ID=0
    elif [ $VOICE_VLAN_TYPE -eq "1" ];then
        let DATA_VLAN_ID=1
    else
        let DATA_VLAN_ID=0
    fi
    # wan vlan enable , lan vlan disable
    let pbmp=$PORT_CPU_BITMAP+$PORT_LAN_BITMAP
    let ubmp=$PORT_CPU_BITMAP
    let port_bit_map0=$PORT_PC_BITMAP+$PORT_LAN_BITMAP
    let port_untag_map0=$PORT_PC_BITMAP+$PORT_LAN_BITMAP
    let port_bit_map1=$PORT_LAN_BITMAP+$PORT_CPU_BITMAP
    let port_add_tag_map1=$PORT_LAN_BITMAP
    let port_bit_map2=$PORT_LAN_BITMAP+$PORT_PC_BITMAP

    if [ $EXTERNAL_SWITCH_NAME == "ip175" ];then
        add_vlan_port_eth0 $VOICE_VLAN_ID $port_bit_map1 $port_add_tag_map1
        add_vlan_port_eth0 $DATA_VLAN_ID $port_bit_map2 $spec_add_tag
        set_untagged_vlan_port_eth0 $PORT_CPU $VOICE_VLAN_ID
        set_untagged_vlan_port_eth0 $PORT_PC $DATA_VLAN_ID
    else
        add_vlan_port_eth0 $VOICE_VLAN_ID $pbmp $ubmp
        add_vlan_port_eth0 $DATA_VLAN_ID $port_bit_map0 $port_untag_map0
        set_untagged_vlan_port_eth0 $PORT_PC $DATA_VLAN_ID
        set_untagged_vlan_port_eth0 $PORT_LAN $DATA_VLAN_ID
        set_untagged_vlan_port_eth0 $PORT_CPU $VOICE_VLAN_ID
    fi
fi

#case  5: voice_vlan_type=1 data_valn_type=2
if [ $VOICE_VLAN_TYPE -eq 1 -a $DATA_VLAN_TYPE -eq 2 ];then

    let pbmp=$PORT_CPU_BITMAP+$PORT_LAN_BITMAP+$PORT_PC_BITMAP
    let ubmp=$PORT_CPU_BITMAP+$PORT_PC_BITMAP
    let port_bit_map0=$PORT_CPU_BITMAP+$PORT_LAN_BITMAP
    let port_untag_map0=$PORT_CPU_BITMAP
    let port_bit_map1=$PORT_CPU_BITMAP+$PORT_LAN_BITMAP+$PORT_PC_BITMAP
    let port_add_tag_map1=$PORT_LAN_BITMAP
    let port_bit_map2=$PORT_PC_BITMAP+$PORT_LAN_BITMAP
    let port_add_tag_map2=$PORT_LAN_BITMAP
    let port_untag_map1=$PORT_PC_BITMAP

    if [ $DATA_VLAN_ID -eq $VOICE_VLAN_ID ];then
        if [ $EXTERNAL_SWITCH_NAME == "ip175" ];then
            add_vlan_port_eth0 $VOICE_VLAN_ID $port_bit_map1 $port_add_tag_map1
            set_untagged_vlan_port_eth0 $PORT_PC $VOICE_VLAN_ID
            set_untagged_vlan_port_eth0 $PORT_CPU $VOICE_VLAN_ID            
        else
            # rtl8304 & rtl8363 [ $DATA_VLAN_ID -eq $VOICE_VLAN_ID ]
            add_vlan_port_eth0 $VOICE_VLAN_ID $pbmp $ubmp
            set_untagged_vlan_port_eth0 $PORT_PC $VOICE_VLAN_ID
            set_untagged_vlan_port_eth0 $PORT_LAN $VOICE_VLAN_ID
            set_untagged_vlan_port_eth0 $PORT_CPU $VOICE_VLAN_ID
        fi
    else
            #  [ $DATA_VLAN_ID -ne $VOICE_VLAN_ID ]
        if [ $EXTERNAL_SWITCH_NAME == "ip175" ];then
            add_vlan_port_eth0 $VOICE_VLAN_ID $port_bit_map0 $port_add_tag_map1
            add_vlan_port_eth0 $DATA_VLAN_ID $port_bit_map2 $port_add_tag_map2
            set_untagged_vlan_port_eth0 $PORT_CPU $VOICE_VLAN_ID
            set_untagged_vlan_port_eth0 $PORT_PC $DATA_VLAN_ID
        else
            # rtl8304 & rtl8363 [ $DATA_VLAN_ID -ne $VOICE_VLAN_ID ]
            add_vlan_port_eth0 $VOICE_VLAN_ID $port_bit_map0 $port_untag_map0
            add_vlan_port_eth0 $DATA_VLAN_ID $port_bit_map2 $port_untag_map1
            set_untagged_vlan_port_eth0 $PORT_PC $DATA_VLAN_ID
            set_untagged_vlan_port_eth0 $PORT_LAN $DATA_VLAN_ID
            set_untagged_vlan_port_eth0 $PORT_CPU $VOICE_VLAN_ID
        fi
    fi
fi

ethtool --set-untagged-vlan-port-priority eth0 port $PORT_PC  priority $DATA_VLAN_PRI 
ethtool --set-untagged-vlan-port-priority eth0 port $PORT_CPU  priority $VOICE_VLAN_PRI
