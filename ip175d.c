#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include "ip175d.h"

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>

MODULE_DESCRIPTION("ICPlus IP175L PHY driver");
MODULE_AUTHOR("fanvil");
MODULE_LICENSE("GPL");

#define TRUE 1
#define	FALSE 0
#define RET_OK 0
#define RET_ERROR -1
#define IP175d_MAX_VID 0xFFF
#define RT_ERR_VLAN_VID 0x100
#define IP175D_MAX_PORTMASK	0x003F
//#define PORTMASK 0x1FF
#define PHY_WAN_PORT 0
#define PHY_LAN_PORT 1
#define PHY_CPU_PORT 5
//#define IP175D_MAX_VLANINDEX 15
//#define MAX_VLANS 16
#define MAX_PORTS 6
#define DIFFERENT_PORT_TAG_FLAG 0
#define LAN_PORT_TAG_FLAG 1
#define WAN_PORT_TAG_FLAG 2
#define DOUBLE_EQUAL_PORT_TAG_FLAG 3
#define RTK_TOTAL_NUM_OF_WORD_FOR_1BIT_PORT_LIST 1

//#define IP175D_VLAN_PROC_DEBUG
#define IP175D_VLAN_ADDR_22 22
#define IP175D_VLAN_ADDR_23 23
#define IP175D_PORT_NUM 6
#define IP175D_ENTRYS_NUM 16
#define IP175D_PORT_VID_REG_START 4
#define IP175D_VALN_ENTRY0_START_REG 14
#define IP175D_VLAN_ENTRYS_ONOFF_REG 10
#define IP175D_VLAN_ENTRYS_PRI_ONOFF_REG 13
#define IP175D_VLAN_ENTRYS_PRI_VAL_START_REG 24
#define IP175D_VALN_ENTRY0_MBR_START_REG 0
#define IP175D_VALN_ENTRY0_ADDTAG_START_REG 8
#define IP175D_VALN_ENTRY0_RMTAG_START_REG 16
#define IP175D_CPU_WAN_PORT 0x21
#define IP175D_WAN_LAN_PORT 0x03
#define IP175D_CPU_WAN_LAN_PORT 0x23

//#define DEBUG_INFO
#if defined(DEBUG_INFO)
#define DBG_INFO(fmt,arg...)          printk(" "fmt"\n",##arg)
#else
#define DBG_INFO(fmt,arg...) ;
#endif

typedef struct ip175d_portmask_s
{
    unsigned int  bits[RTK_TOTAL_NUM_OF_WORD_FOR_1BIT_PORT_LIST];
} ip175d_portmask_t;

struct switch_port_info{
    rtk_port_linkStatus_t status;
    rtk_port_speed_t speed;
    rtk_port_duplex_t duplex;
};

unsigned int remNum = 0;

static struct switch_port_info g_switch_lan_info = {0};
static struct switch_port_info g_switch_wan_info = {0};
static struct proc_dir_entry *g_switch_dir = NULL;
static struct proc_dir_entry *g_switch_lan_dir = NULL;
static struct proc_dir_entry *g_switch_wan_dir = NULL;
#ifdef IP175D_VLAN_PROC_DEBUG
static struct proc_dir_entry *g_switch_vlan_dir = NULL;
static struct net_device *g_net_dev  = NULL;
#endif

static void vlan_set_shift_val(unsigned int is_high_bit, struct mii_bus *bus, int addr, u32 regnum, unsigned int new_val){
    unsigned int reg_val = 0;

    reg_val = mdiobus_read(bus, addr, regnum);
    DBG_INFO("addr:%d, reg num:%d original reg_val: 0x%02x \n",addr, regnum, reg_val);

    if(is_high_bit){
        //高八位, 将mbrmask 放到reg的高8位上
        reg_val &= 0x00FF;//将高8位置为0000 0000
        reg_val |= ((new_val<< 8) & 0xFF00);//将低8位置为0000 0000
    }else{
        //低八位，将mbrmask 放到reg的低8位上
        reg_val &= 0xFF00;//将低8位置为0000 0000
        reg_val |= (new_val & 0x00FF);//将高8位置为0000 0000
    }

    DBG_INFO("addr:%d, reg num:%d new reg_val: 0x%02x \n",addr, regnum, reg_val);
    mdiobus_write(bus, addr, regnum, reg_val);
}

static int vlan_get_shift_val(unsigned int is_high_bit, struct mii_bus *bus, int addr, u32 regnum){
    unsigned int reg_val = 0;

    reg_val = mdiobus_read(bus, addr, regnum);
    DBG_INFO("original reg_val: 0x%02x addr:%d, reg num:%d\n",reg_val, addr, regnum);

    if(is_high_bit){
        reg_val = (reg_val & 0xFF00) >> 8;
    }else{
        reg_val &= 0x00FF;
    }
    DBG_INFO("is high bit:%d, new reg_val: 0x%02x addr:%d, reg num:%d\n",is_high_bit, reg_val, addr, regnum);

    return reg_val;
}

static int ip175d_vlan_entry_set(struct net_device *dev, unsigned int vlan_entry_index, unsigned int v_id, unsigned int mbr_mask, unsigned int remove_tag_mask, unsigned int add_tag_mask)
{
    unsigned int vlan_entry_vid_reg = 0, vlan_entry_mbr_reg = 0;
    unsigned int vlan_entry_add_tag_reg = 0, vlan_entry_remove_tag_reg = 0;

    /* set cur vlan entry vid */
    vlan_entry_vid_reg = IP175D_VALN_ENTRY0_START_REG + vlan_entry_index;
    DBG_INFO("vlan_entry_reg: %d v_id:%d mbr_mask:%02x\n",vlan_entry_vid_reg, v_id, mbr_mask);
    mdiobus_write(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, vlan_entry_vid_reg, v_id);

    /* set cur vlan entry members */
    vlan_entry_mbr_reg = IP175D_VALN_ENTRY0_MBR_START_REG + vlan_entry_index/2;
    vlan_set_shift_val(vlan_entry_index%2, dev->phydev->mdio.bus, IP175D_VLAN_ADDR_23, vlan_entry_mbr_reg, mbr_mask);

    /* set cur vlan entry add tag*/
    vlan_entry_add_tag_reg = IP175D_VALN_ENTRY0_ADDTAG_START_REG + vlan_entry_index/2;
    vlan_set_shift_val(vlan_entry_index%2, dev->phydev->mdio.bus, IP175D_VLAN_ADDR_23, vlan_entry_add_tag_reg, add_tag_mask);

    /* set cur vlan entry remove tag*/
    vlan_entry_remove_tag_reg = IP175D_VALN_ENTRY0_RMTAG_START_REG + vlan_entry_index/2;
    vlan_set_shift_val(vlan_entry_index%2, dev->phydev->mdio.bus, IP175D_VLAN_ADDR_23, vlan_entry_remove_tag_reg, remove_tag_mask);

    return RET_OK;
}

static int ip175d_vlan_entry_get(struct net_device *dev, unsigned int vlan_entry_index){
    unsigned int reg_val = 0;
    unsigned int vlan_entry_mbr_reg = 0, vlan_entry_add_tag_reg = 0, vlan_entry_remove_tag_reg = 0;;

    /* set cur vlan entry members */
    vlan_entry_mbr_reg = IP175D_VALN_ENTRY0_MBR_START_REG + vlan_entry_index/2;
    reg_val = vlan_get_shift_val(vlan_entry_index%2, dev->phydev->mdio.bus, IP175D_VLAN_ADDR_23, vlan_entry_mbr_reg);
    printk("%s:mbr mask:0x%02x!\n", __func__, reg_val);

    /* set cur vlan entry add tag*/
    vlan_entry_add_tag_reg = IP175D_VALN_ENTRY0_ADDTAG_START_REG + vlan_entry_index/2;
    reg_val = vlan_get_shift_val(vlan_entry_index%2, dev->phydev->mdio.bus, IP175D_VLAN_ADDR_23, vlan_entry_add_tag_reg);
    printk("%s:add tag port:0x%02x!\n", __func__, reg_val);

    /* set cur vlan entry remove tag*/
    vlan_entry_remove_tag_reg = IP175D_VALN_ENTRY0_RMTAG_START_REG + vlan_entry_index/2;
    reg_val = vlan_get_shift_val(vlan_entry_index%2, dev->phydev->mdio.bus, IP175D_VLAN_ADDR_23, vlan_entry_remove_tag_reg);
    printk("%s:remove tag port:0x%02x!\n", __func__, reg_val);

    return RET_OK;
}

static unsigned int ip175d_search_vlan_entry(struct net_device *dev, unsigned int vid){
    unsigned int index =0, reg_num = 0 ,reg_val = 0;
    for(index = 0; index < IP175D_ENTRYS_NUM; index++){
        reg_num = IP175D_VALN_ENTRY0_START_REG + index;
        reg_val = mdiobus_read(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, reg_num);
        if(reg_val == vid || reg_val == 0){
            break;
        }
    }
    return index;
}

static int ip175d_vlan_set(struct net_device *dev, unsigned int vid, unsigned int  mbr_mask,unsigned int  untag_mask){
    unsigned int vlan_entry_index = 0, reg_val = 0, remove_tag_mask = 0, add_tag_mask = 0;
    unsigned vlan_onoff_reg = 0;
	
    vlan_entry_index = ip175d_search_vlan_entry(dev,vid);
	
    remove_tag_mask = untag_mask;
    add_tag_mask = mbr_mask - untag_mask;
    ip175d_vlan_entry_set(dev, vlan_entry_index, vid, mbr_mask, remove_tag_mask, add_tag_mask);

    //set enable vlan
    vlan_onoff_reg = IP175D_VLAN_ENTRYS_ONOFF_REG;
    reg_val = mdiobus_read(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, vlan_onoff_reg);
    reg_val |= (1 << vlan_entry_index);
    mdiobus_write(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, vlan_onoff_reg, reg_val);

    return RET_OK;
}

int ip175_get_vlan_port(struct net_device *dev, struct ethtool_vlan_port *port){
    unsigned int index =0, reg_num = 0, reg_val =0 ;

    if(dev == NULL || port == NULL){
        printk(KERN_ERR "%s:dev or port is null!", __func__);
        return -1;
    }

    printk("%s:get vid:0x%02x info!\n", __func__, port->vid);
    for(index = 0; index < IP175D_ENTRYS_NUM; index++){
        reg_num = IP175D_VALN_ENTRY0_START_REG + index;
        reg_val = mdiobus_read(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, reg_num);
        if(reg_val == port->vid){
            DBG_INFO("cur vlan entry reg: %d vlan entry index:%d reg val:0x%02x vid:%d\n",reg_num, index, reg_val, port->vid);
            break;
        }
    }
    ip175d_vlan_entry_get(dev, index);

    for(index = 0; index < IP175D_PORT_NUM; index++){
        reg_num = IP175D_PORT_VID_REG_START + index;
        reg_val = mdiobus_read(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, reg_num);
        if(reg_val == port->vid){
            printk("%s:get vid:0x%02x info port:0x%d!\n", __func__, port->vid, index);
        }
    }
    return 0;
}

int ip175_add_vlan_port(struct net_device *dev, struct ethtool_vlan_port *port){
    int ret = RET_ERROR;

    if(dev == NULL || port == NULL){
        printk(KERN_ERR "%s:dev or port is null!\n", __func__);
        return -1;
    }

    /* vid 2 ~ 4094 */
    if((port->vid == 0) ||port->vid >= IP175d_MAX_VID){
        printk(KERN_ERR "%s:port VID invalid:0x%02x\n", __func__, port->vid);
        return -2;
    }

    ret = ip175d_vlan_set(dev, port->vid, port->Mbrmsk, port->Untagmsk);
	
    if(ret == RET_OK){
        printk("vlan add port succeed: vid=%d, mbr=%x, untagmask:%x\n",port->vid, port->Mbrmsk, port->Untagmsk);
    }else{
        printk(KERN_ERR "vlan add port failed(0x%x): vid=%d, mbr=%x, untagmask:%x\n", ret, port->vid, port->Mbrmsk, port->Untagmsk);
    }
    return RET_OK;
}

int ip175_remove_vlan_port(struct net_device *dev, struct ethtool_vlan_port *port){
    unsigned int index =0, reg_num = 0, reg_val =0 ;

    if(dev == NULL || port == NULL){
        printk(KERN_ERR "%s:dev or port is null!", __func__);
        return -1;
    }

    printk("%s:get vid:0x%02x info!\n", __func__, port->vid);
    for(index = 0; index < IP175D_ENTRYS_NUM; index++){
        reg_num = IP175D_VALN_ENTRY0_START_REG + index;
        reg_val = mdiobus_read(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, reg_num);
        if(reg_val == port->vid){
            DBG_INFO("cur vlan entry reg: %d vlan entry index:%d reg val:0x%02x vid:%d\n",reg_num, index, reg_val, port->vid);
            break;
        }
    }
    ip175d_vlan_entry_set(dev, index, 0, 0x3f3f, 0, 0);

    for(index = 0; index < IP175D_PORT_NUM; index++){
        reg_num = IP175D_PORT_VID_REG_START + index;
        reg_val = mdiobus_read(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, reg_num);
        if(reg_val == port->vid){
            mdiobus_write(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, reg_num, 1);
        }
    }

    return 0;
}

int ip175_set_untagged_vlan_port(struct net_device *dev, struct ethtool_untagged_vlan_port * uport){
    unsigned int port_vid_reg_num = 0;

    if(dev == NULL || uport == NULL){
        printk(KERN_ERR "%s:dev or port is null!", __func__);
        return -1;
    }

    if (uport->port >= MAX_PORTS){
        printk(KERN_ERR "%s:port:%d is too large!", __func__, uport->port );
        return -2;
    }

    //set port vid, if have untagged frame received, use pvid search vlan table 
    port_vid_reg_num = IP175D_PORT_VID_REG_START + uport->port;
    DBG_INFO("port_vid_reg_num: %d pvid:0x%02x \n",port_vid_reg_num, uport->vid);
    mdiobus_write(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, port_vid_reg_num, uport->vid);
    printk("vlan set port untagged succeed: port=%d, vid=%d\n",uport->port, uport->vid);

    return 0;
}

int ip175_get_untagged_vlan_port(struct net_device *dev, struct ethtool_untagged_vlan_port *uport){
    unsigned int port_vid_reg_num = 0, port_vid_reg_val = 0;

    if(dev == NULL || uport == NULL){
        printk(KERN_ERR "%s:dev or port is null!", __func__);
        return -1;
    }

    if (uport->port >= MAX_PORTS){
        printk(KERN_ERR "%s:port:%d is too large!", __func__, uport->port );
        return -2;
    }

    //set port vid, if have untagged frame received, use pvid search vlan table 
    port_vid_reg_num = IP175D_PORT_VID_REG_START + uport->port ;
    port_vid_reg_val = mdiobus_read(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, port_vid_reg_num);
    printk("port %d pvid: 0x%02x\n",uport->port,port_vid_reg_val);

    return 0;
}


static int ip175d_vlan_entry_set_pri(struct net_device *dev, unsigned int vlan_entry_index, unsigned int pri){
    unsigned int pri_onoff_reg = 0, pri_val_reg = 0, reg_val = 0;

    /* enable cur vlan entry pri */
    pri_onoff_reg = IP175D_VLAN_ENTRYS_PRI_ONOFF_REG;
    reg_val = mdiobus_read(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, pri_onoff_reg);
    DBG_INFO("addr:%d pri_onoff_reg: %d ori reg_val:%02x\n", IP175D_VLAN_ADDR_22, pri_onoff_reg, reg_val);

    reg_val |= (1 << vlan_entry_index);
    DBG_INFO("addr:%d pri_onoff_reg: %d new reg_val:%02x\n", IP175D_VLAN_ADDR_22, pri_onoff_reg, reg_val);
    mdiobus_write(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, pri_onoff_reg, reg_val);

    /* set cur vlan entry pri value */
    pri_val_reg = IP175D_VLAN_ENTRYS_PRI_VAL_START_REG + vlan_entry_index/2;
    reg_val = pri << 5;
    vlan_set_shift_val(vlan_entry_index%2, dev->phydev->mdio.bus, IP175D_VLAN_ADDR_23, pri_val_reg, reg_val);


    return 0;
}

static int ip175d_vlan_entry_get_pri(struct net_device *dev, unsigned int vlan_entry_index){
    unsigned int pri_val_reg = 0;

    /* get cur vlan entry pri value */
    pri_val_reg = IP175D_VLAN_ENTRYS_PRI_VAL_START_REG + vlan_entry_index/2;
    vlan_get_shift_val(vlan_entry_index%2, dev->phydev->mdio.bus, IP175D_VLAN_ADDR_23, pri_val_reg);

    return 0;
}

int ip175_set_untagged_vlan_port_priority(struct net_device *dev, struct ethtool_untagged_port_priority *ppriority){

    unsigned int vlan_entry_index = 0;
    unsigned int reg_val = 0, bit_val = 0;
    /* set cur vlan entry members */

    if(dev == NULL || ppriority == NULL){
        printk(KERN_ERR "%s:dev or ppriority is null!", __func__);
        return -1;
    }

    //skip the entry 0[default vid 1], find the enable entry, set pri value in it
    reg_val = mdiobus_read(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, IP175D_VLAN_ENTRYS_ONOFF_REG);
    for(vlan_entry_index = 1; vlan_entry_index < IP175D_ENTRYS_NUM; vlan_entry_index++){
        bit_val = reg_val & (1 << vlan_entry_index);
        if(bit_val){
            ip175d_vlan_entry_set_pri(dev, vlan_entry_index, ppriority->priority);
            break;
        }
    }
    return 0;
}

int ip175_get_untagged_vlan_port_priority(struct net_device * dev, struct ethtool_untagged_port_priority *ppriority){
    unsigned int index =0, vlan_entry_index = 0;
    unsigned int reg_val = 0, bit_val = 0;
    /* set cur vlan entry members */

    if(dev == NULL || ppriority == NULL){
        printk(KERN_ERR "%s:dev or ppriority is null!", __func__);
        return -1;
    }

    reg_val = mdiobus_read(dev->phydev->mdio.bus, IP175D_VLAN_ADDR_22, IP175D_VLAN_ENTRYS_PRI_ONOFF_REG);
    //skip the entry 0[default vid 1], find the enable entry, set pri value in it
    for(vlan_entry_index = 1; vlan_entry_index < IP175D_ENTRYS_NUM; vlan_entry_index++){
        bit_val = reg_val & (1 << vlan_entry_index);
        if(bit_val){
            ip175d_vlan_entry_get_pri(dev, index);
            break;
        }
    }
    return 0;
}

int ip175_init_vlan(struct net_device *dev, struct ethtool_vlan_port *port){

    int i;
    /* VLAN classification rules: tag-based VLANs, use VID to classify, clear vlan table
    drop packets that cannot be classified.
    port 0 ~ 5 set tag-based VLANs   
    P1 P5   pvid classification always use PVID to search VLAN table
    P0 VID classification use VID to search VLAN table if tag packet use PVID to search VLAN table if untag packet*/
    mdiobus_write(dev->phydev->mdio.bus, 22, 0, 0x0fbf);//

    /* Ingress rules: CFI=1 dropped, null VID is untagged, VID=1 passed,
    VID=0xfff discarded, admin both tagged and untagged, ingress,admit all packet
    filters enabled.															*/
    mdiobus_write(dev->phydev->mdio.bus, 22, 1, 0x0c3f);

    /* Egress rules: IGMP processing off, keep VLAN header off */
    mdiobus_write(dev->phydev->mdio.bus, 22, 2, 0x0000);

    /* clear vlan table; vid = 0  */
    for(i = 0; i < 16; i++)
        mdiobus_write(dev->phydev->mdio.bus, 22, 14+i, 0x0000);


    /* clear vlan entry; vid = 0  */
    for(i = 0; i < 8; i++) {
        mdiobus_write(dev->phydev->mdio.bus, 23, i, 0x3f3f);//VLAN Member Default
        mdiobus_write(dev->phydev->mdio.bus, 23, 8+i, 0x0000);//VLAN Add Tag Default
        mdiobus_write(dev->phydev->mdio.bus, 23, 16+i, 0x0000);//VLAN Remove Tag  Default
        mdiobus_write(dev->phydev->mdio.bus, 23, 24+i, 0x0000);//VLAN Miscellaneous Register Default (VLAN PRIORITY)
    }

    /*defalut port pvid 1, set entry0 for default pvid vlan table*/
    //enatble entry0
    mdiobus_write(dev->phydev->mdio.bus, 22, 10, 0x0001);
    //set entry0 default vid 1
    mdiobus_write(dev->phydev->mdio.bus, 22, 14, 0x0001);
    //enatble entry0 member wan+cpu+lan
    mdiobus_write(dev->phydev->mdio.bus, 23, 0, 0x0023);

#ifdef IP175D_VLAN_PROC_DEBUG
    g_net_dev = dev;
#endif
    return 0;
}

int ip175_uninit_vlan(struct net_device *dev, struct ethtool_vlan_port *port){
    int i = 0;

    /* VLAN Classification Default */
    mdiobus_write(dev->phydev->mdio.bus, 22, 0, 0x0000);

    /* PVID Default */
    for(i = 0; i < 6; i++)
        mdiobus_write(dev->phydev->mdio.bus, 22, 4+i, 0x0001);

    /* VLAN_VALID set 0 Default */
    mdiobus_write(dev->phydev->mdio.bus, 22, 10, 0x0000);

    /* VLAN_PRI ( REW_VLAN_PRI_EN ) set 0 Default */
    mdiobus_write(dev->phydev->mdio.bus, 22, 13, 0x0000);

    for(i = 0; i < 8; i++) {
        mdiobus_write(dev->phydev->mdio.bus, 23, i, 0x3f3f);//VLAN Member Default
        mdiobus_write(dev->phydev->mdio.bus, 23, 8+i, 0x0000);//VLAN Add Tag Default
        mdiobus_write(dev->phydev->mdio.bus, 23, 16+i, 0x0000);//VLAN Remove Tag  Default
        mdiobus_write(dev->phydev->mdio.bus, 23, 24+i, 0x0000);//VLAN Miscellaneous Register Default (VLAN PRIORITY)
    }

    return 0;
}

static int _speed_proc_show(struct seq_file *seq, void *v, rtk_port_speed_t speed)
{
    switch(speed){
        case PORT_SPEED_10M:
            seq_printf(seq, "10\n");
            break;
        case PORT_SPEED_100M:
            seq_printf(seq, "100\n");
            break;
        case PORT_SPEED_1000M:
            seq_printf(seq, "1000\n");
            break;
        default:
            seq_printf(seq, "\n");
            break;
    }
    return 0;
}

static int _link_status_proc_show(struct seq_file *seq, void *v, rtk_port_linkStatus_t status)
{
    switch(status){
        case PORT_LINKDOWN:
            seq_printf(seq, "down\n");
            break;
        case PORT_LINKUP:
            seq_printf(seq, "up\n");
            break;
        default:
            seq_printf(seq, "\n");
            break;
    }
    return 0;
}

static int _duplex_proc_show(struct seq_file *seq, void *v, rtk_port_duplex_t duplex){
    switch(duplex){
        case PORT_HALF_DUPLEX:
            seq_printf(seq, "half\n");
            break;
        case PORT_FULL_DUPLEX:
            seq_printf(seq, "full\n");
            break;
        default:
            seq_printf(seq, "\n");
            break;
    }
    return 0;
}

static int lan_speed_proc_show(struct seq_file *seq, void *v){
    return _speed_proc_show(seq, v, g_switch_lan_info.speed);
}

static int lan_status_proc_show(struct seq_file *seq, void *v){
    return _link_status_proc_show(seq, v, g_switch_lan_info.status);
}

static int lan_duplex_proc_show(struct seq_file *seq, void *v)
{
    return _duplex_proc_show(seq, v, g_switch_lan_info.duplex);
}

static int wan_speed_proc_show(struct seq_file *seq, void *v){
    return _speed_proc_show(seq, v, g_switch_wan_info.speed);
}

static int wan_status_proc_show(struct seq_file *seq, void *v){
    return _link_status_proc_show(seq, v, g_switch_wan_info.status);
}

static int wan_duplex_proc_show(struct seq_file *seq, void *v)
{
    return _duplex_proc_show(seq, v, g_switch_wan_info.duplex);
}


static int lan_speed_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, lan_speed_proc_show, NULL);
}

static int lan_status_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, lan_status_proc_show, NULL);
}

static int lan_duplex_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, lan_duplex_proc_show, NULL);
}

static int wan_speed_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, wan_speed_proc_show, NULL);
}

static int wan_status_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, wan_status_proc_show, NULL);
}

static int wan_duplex_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, wan_duplex_proc_show, NULL);
}

static const struct file_operations lan_speed_fops = {
    .open       = lan_speed_proc_open,
    .read       = seq_read,
    .release    = single_release,
};

static const struct file_operations lan_status_fops = {
    .open       = lan_status_proc_open,
    .read       = seq_read,
    .release    = single_release,
};

static const struct file_operations lan_duplex_fops = {
    .open       = lan_duplex_proc_open,
    .read       = seq_read,
    .release    = single_release,
};

static const struct file_operations wan_speed_fops = {
    .open       = wan_speed_proc_open,
    .read       = seq_read,
    .release    = single_release,
};

static const struct file_operations wan_status_fops = {
    .open       = wan_status_proc_open,
    .read       = seq_read,
    .release    = single_release,
};

static const struct file_operations wan_duplex_fops = {
    .open       = wan_duplex_proc_open,
    .read       = seq_read,
    .release    = single_release,
};

#ifdef IP175D_VLAN_PROC_DEBUG
static int vlan_reg_proc_show(struct seq_file *seq, void *v){
    seq_printf(seq, "please read and write this file by fopen!!\n");
    return 0;
}

static int vlan_reg_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, vlan_reg_proc_show, NULL);
}

static int vlan_reg_proc_read(struct file *file, char __user *buffer,size_t count, loff_t *ppos){
    char buf[1024];
    int index = 0;
    int len, num,reg_addr,reg_addr_index, reg_val;

    if(copy_from_user(buf, buffer, count))
        return -EFAULT;

    num = sscanf(buf,"%d %d",&reg_addr,&reg_addr_index);
    if(num != 2)
        return -EFAULT;
    if(!g_net_dev){
        printk("==%s=g_net_dev is null! please run [ethtool --init-vlan eth0] first==\n", __func__);
        return 0;
    }
    if(reg_addr == 1){
        for(index=0;index<32;index++){
            mdiobus_read(g_net_dev->phydev->mdio.bus, 22, index);
        }
        for(index=0;index<32;index++){
            mdiobus_read(g_net_dev->phydev->mdio.bus, 23, index);
        }
    }else{
        reg_val = mdiobus_read(g_net_dev->phydev->mdio.bus, reg_addr, reg_addr_index);
    }

    len += sprintf(buf,"reg_val = 0x%02x\n",reg_val);
    
    if(copy_to_user(buffer,buf,len))
        return -EFAULT;
    *ppos = len;
    return len;
}

static int vlan_reg_proc_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos){
    int num,reg_addr,reg_addr_index, reg_val;
    char buf[1024];
    if(*pos > 0 || count > 1024)
        return -EFAULT;
    if(copy_from_user(buf, buffer, count))
        return -EFAULT;
    num = sscanf(buf,"%d %d %04x",&reg_addr,&reg_addr_index, &reg_val);
    if(num != 3)
        return -EFAULT;
    if(!g_net_dev){
        printk("==%s=g_net_dev is null! please run [ethtool --init-vlan eth0] first==\n", __func__);
        return 0;
    }
    mdiobus_write(g_net_dev->phydev->mdio.bus, reg_addr, reg_addr_index, reg_val);
    return 0;
}

static const struct file_operations vlan_reg_fops = {
    .open       = vlan_reg_proc_open,
    .read       = vlan_reg_proc_read,
    .write  =  vlan_reg_proc_write,
    .release    = single_release,
};
#endif

static int ip175d_config_init(struct phy_device *phydev)
{
    int err, i;
    static int full_reset_performed;

    if (full_reset_performed == 0) {

        err = mdiobus_read(phydev->mdio.bus, 20, 0);

        if (err == 0x175d) {    //IP175D
            err = mdiobus_write(phydev->mdio.bus, 20, 2, 0x175d);
            if (err < 0)
				return err;

            err = mdiobus_read(phydev->mdio.bus, 20, 2);
            mdelay(2);

            err = mdiobus_write(phydev->mdio.bus, 20, 4, 0xa000);
            if (err < 0)
                return err;
        } else {    //IP175C
            err = mdiobus_write(phydev->mdio.bus, 30, 0, 0x175c);
            if (err < 0)
                return err;

            err = mdiobus_read(phydev->mdio.bus, 30, 0);
            mdelay(2);

            err = mdiobus_write(phydev->mdio.bus, 29, 31, 0x175c);
            if (err < 0)
                return err;

            err = mdiobus_write(phydev->mdio.bus, 29, 22, 0x420);
            if (err < 0)
                return err;
        }

        for (i = 0; i < 5; i++) {
            err = mdiobus_write(phydev->mdio.bus, i, MII_BMCR,
            (BMCR_RESET | BMCR_SPEED100 | BMCR_ANENABLE | BMCR_FULLDPLX));
            if (err < 0)
                return err;
        }

        for (i = 0; i < 5; i++)
            err = mdiobus_read(phydev->mdio.bus, i, MII_BMCR);

        mdelay(2);

        full_reset_performed = 1;
    }

    if (phydev->mdio.addr != 4) {
        phydev->state = PHY_RUNNING;
        phydev->speed = SPEED_100;
        phydev->duplex = DUPLEX_FULL;
        phydev->link = 1;
        netif_carrier_on(phydev->attached_dev);
    }
	
    return 0;
}

int ip175d_port_phyStatus_get(unsigned int phy, rtk_port_linkStatus_t *pLinkStatus, rtk_port_speed_t *pSpeed, rtk_port_duplex_t *pDuplex, struct phy_device *phydev)
{
    int regVal;
    if(phy > (MAX_PORTS-1))
		return RET_ERROR;
    if(NULL == pLinkStatus)
		return RET_ERROR;

    regVal = mdiobus_read(phydev->mdio.bus, phy, 1);
    //DBG_INFO("[ %d.1 ]: 0x%x",phy,regVal);
    //regVal = mdiobus_read(phydev->mdio.bus, phy, 18);
    if(regVal & 0x4)//Link Status
    {
        *pLinkStatus = PORT_LINKUP;
        *pSpeed = PORT_SPEED_100M;
        *pDuplex = PORT_FULL_DUPLEX;
    }
    else
    {
        *pLinkStatus = PORT_LINKDOWN;
        *pSpeed = PORT_SPEED_10M;
        *pDuplex = PORT_HALF_DUPLEX;
    }

    return RET_OK;
}

static int ip175d_read_status(struct phy_device *phydev)
{
    ip175d_port_phyStatus_get(PHY_LAN_PORT,&(g_switch_lan_info.status), &(g_switch_lan_info.speed), &(g_switch_lan_info.duplex), phydev);
    ip175d_port_phyStatus_get(PHY_WAN_PORT,&(g_switch_wan_info.status), &(g_switch_wan_info.speed), &(g_switch_wan_info.duplex), phydev);
	
    genphy_read_status(phydev);

    return 0;
}

static int ip175d_config_aneg(struct phy_device *phydev)
{

    if (phydev->mdio.addr == 4)
        genphy_config_aneg(phydev);

    return 0;
}

static struct phy_driver ip175d_driver = {
    .phy_id		= 0x001cc943,
    .name		= "ICPlus IP175d",
    .phy_id_mask	= 0x0ffffff0,
    .features	= (PHY_BASIC_FEATURES | SUPPORTED_Pause | SUPPORTED_Asym_Pause),
    .flags		= PHY_HAS_INTERRUPT,
    .config_init	= ip175d_config_init,
    .config_aneg	= ip175d_config_aneg,
    .read_status	= ip175d_read_status,
    .suspend	= genphy_suspend,
    .resume		= genphy_resume,
};

struct ethtool_ops sstar_emac_ethtool_ops = {
/*	.get_link_ksettings	= sstar_emac_get_link_ksettings,
	.set_link_ksettings	= sstar_emac_set_link_ksettings,
	// .get_drvinfo		= sstar_emac_get_drvinfo,
	// .get_msglevel		= sstar_emac_get_msglevel,
	// .set_msglevel		= sstar_emac_set_msglevel,
	.nway_reset		= sstar_emac_nway_reset,
	.get_link		= sstar_emac_get_link,
	// .get_strings		= sstar_emac_get_strings,
	// .get_sset_count		= sstar_emac_get_sset_count,
	// .get_ethtool_stats	= sstar_emac_get_ethtool_stats,
	// .get_rxnfc		= sstar_emac_get_rxnfc,
	// .set_rxnfc              = sstar_emac_set_rxnfc,
*/
#ifdef CONFIG_ETHTOOL_VLAN
    .init_vlan 			=   ip175_init_vlan,
    .uninit_vlan 		= 	ip175_uninit_vlan,
    .get_vlan_port  	= 	ip175_get_vlan_port,
    .add_vlan_port 		=  	ip175_add_vlan_port,
    .remove_vlan_port	= 	ip175_remove_vlan_port,
    .set_untagged_vlan_port = ip175_set_untagged_vlan_port,
    .get_untagged_vlan_port = ip175_get_untagged_vlan_port,
    .set_untagged_vlan_port_priority = ip175_set_untagged_vlan_port_priority,
    .get_untagged_vlan_port_priority = ip175_get_untagged_vlan_port_priority,
    //.set_dscp_priority = ip175_set_dscp_priority,
    //.get_dscp_priority = ip175_get_dscp_priority,
#endif
};
EXPORT_SYMBOL(sstar_emac_ethtool_ops);


static int __init ip175d_init(void)
{
    int ret = 0;

    g_switch_dir = proc_mkdir("switch", NULL);
    g_switch_lan_dir = proc_mkdir("lan", g_switch_dir);
    proc_create("speed", 0664, g_switch_lan_dir, &lan_speed_fops);
    proc_create("status", 0664, g_switch_lan_dir, &lan_status_fops);
    proc_create("duplex", 0664, g_switch_lan_dir, &lan_duplex_fops);

    g_switch_wan_dir = proc_mkdir("wan", g_switch_dir);
    proc_create("speed", 0664, g_switch_wan_dir, &wan_speed_fops);
    proc_create("status", 0664, g_switch_wan_dir, &wan_status_fops);
    proc_create("duplex", 0664, g_switch_wan_dir, &wan_duplex_fops);

#ifdef IP175D_VLAN_PROC_DEBUG
    g_switch_vlan_dir = proc_mkdir("vlan", g_switch_dir);
    proc_create("reg", 0664, g_switch_vlan_dir, &vlan_reg_fops);
#endif
    ret = phy_driver_register(&ip175d_driver,THIS_MODULE);

    return ret;
}

static void __exit ip175d_exit(void)
{
    remove_proc_entry("speed", g_switch_lan_dir);
    remove_proc_entry("status", g_switch_lan_dir);
    remove_proc_entry("duplex", g_switch_lan_dir);
    proc_remove(g_switch_lan_dir);

    remove_proc_entry("speed", g_switch_wan_dir);
    remove_proc_entry("status", g_switch_wan_dir);
    remove_proc_entry("duplex", g_switch_wan_dir);
    proc_remove(g_switch_wan_dir);    
    
    proc_remove(g_switch_dir);
    phy_driver_unregister(&ip175d_driver);
}

module_init(ip175d_init);
module_exit(ip175d_exit);

static struct mdio_device_id __maybe_unused ip175_tbl[] = {
    { 0x001cc943, 0x0ffffff0 },
    { }
};

MODULE_DEVICE_TABLE(mdio, ip175_tbl);

