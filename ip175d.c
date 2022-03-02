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

int ip175_init_vlan(struct net_device *dev, struct ethtool_vlan_port *port)
{
    int i;
    /* VLAN classification rules: tag-based VLANs, use VID to classify, clear vlan table
    drop packets that cannot be classified.
    port 0 ~ 5 set tag-based VLANs   
    P1 P5   pvid classification   P0 VID classification */
    mdiobus_write(dev->phydev->mdio.bus, 22, 0, 0x0fbf);//003f

    /* Ingress rules: CFI=1 dropped, null VID is untagged, VID=1 passed,
    VID=0xfff discarded, admin both tagged and untagged, ingress,admit all packet
    filters enabled.															*/
    mdiobus_write(dev->phydev->mdio.bus, 22, 1, 0x0c3f);

    /* Egress rules: IGMP processing off, keep VLAN header off */
    mdiobus_write(dev->phydev->mdio.bus, 22, 2, 0x0000);

    /* clear vlan table; vid = 0  */
    for(i = 0; i < 16; i++)
        mdiobus_write(dev->phydev->mdio.bus, 22, 14+i, 0x0000);

    /*defalut port pvid 1*/

    return RET_OK;
}

int ip175d_vlan_entry_get(unsigned int vlanIndex, unsigned int *portVid, unsigned int *portMbrmsk, unsigned int *portUntamsk, unsigned int *portfid, struct net_device *dev)
{
    int regVal;
    unsigned int regNum;

    if ((vlanIndex > 15) || (NULL == portVid) || (NULL == portMbrmsk) || (NULL == portUntamsk) || (NULL == portfid))
        return RET_ERROR;

    regNum = 14 + vlanIndex;
    /* vlan info --include fid and vlan */
    regVal = mdiobus_read(dev->phydev->mdio.bus, 22, regNum);
    *portVid = regVal & 0xFFF;
    *portfid = (regVal >> 12) & 0xf;
    /* mbrmsk  and Add-tagmask ---- get vlan member and add tag info */
    if(vlanIndex % 2 == 0)
    {
        regVal = mdiobus_read(dev->phydev->mdio.bus, 23, vlanIndex/2);
        *portMbrmsk = regVal & IP175D_MAX_PORTMASK;

        regVal = mdiobus_read(dev->phydev->mdio.bus, 23, 8 + (vlanIndex/2));
        *portUntamsk = regVal & 0x3F;
        DBG_INFO("VLAN_%d , vid : 0x%x  portMbrmsk : 0x%x, portUntamsk(add-tag) : 0x%x\n",vlanIndex,*portVid,*portMbrmsk,*portUntamsk);
    }
    else{
        regVal = mdiobus_read(dev->phydev->mdio.bus, 23, vlanIndex/2);
        regVal = (regVal >> 8);
        *portMbrmsk = regVal & IP175D_MAX_PORTMASK;

        regVal = mdiobus_read(dev->phydev->mdio.bus, 23, 8 + (vlanIndex/2));
        regVal = (regVal >> 8);
        *portUntamsk = regVal & 0x3F;
        DBG_INFO("VLAN_%d , vid : 0x%x  portMbrmsk : 0x%x, portUntamsk(add-tag) : 0x%x\n",vlanIndex,*portVid,*portMbrmsk,*portUntamsk);
    }

    return RET_OK;
}

int ip175d_vlan_get(unsigned int vid, ip175d_portmask_t *mbrmsk, ip175d_portmask_t *untagmsk, unsigned int *pFid, struct net_device *dev)
{
    int i;
    int hit_flag;
    unsigned int vid_val, mbrmsk_val, untagmsk_val, fid_val;

    /* vid must be 1~4094 */
    if ((vid == 0) || (vid > (IP175d_MAX_VID -1)))
        return RET_ERROR;

    /*seach the vlan table*/
    hit_flag = FALSE;
    for (i = 15; i >= 0; i--)
    {
        ip175d_vlan_entry_get(i, &vid_val, &mbrmsk_val, &untagmsk_val, &fid_val, dev);
        if(vid_val == vid)
        {
            hit_flag = TRUE;
            mbrmsk->bits[0] = mbrmsk_val;
            untagmsk->bits[0] = untagmsk_val;
            *pFid = fid_val;
            //return RET_OK;
        }
    }

    if(!hit_flag)
        return -1;

    return RET_OK;
}

int ip175_get_vlan_port(struct net_device *dev, struct ethtool_vlan_port *port)
{
    ip175d_portmask_t mbrmsk, untagmsk;
    unsigned int fid;
    int ret, regVal, i;

    /* Pvid */
    for(i = 0; i < 6; i++)
    {
        regVal = mdiobus_read(dev->phydev->mdio.bus, 22, 4+i);
        DBG_INFO("[VLAN_INFO_%d] Port %d Pvid：%d\n",i,i,regVal);
    }

    /* get Vlan table */
    ret = ip175d_vlan_get((unsigned int)port->vid, &mbrmsk, &untagmsk, &fid, dev);
    if(ret == RET_OK)
    {
        port->Mbrmsk = mbrmsk.bits[0];
        port->Untagmsk = untagmsk.bits[0];
        DBG_INFO("vlan get port succeed: vid=%d Mbrmsk = %d Add-tagmsk = %d\n",
                port->vid, port->Mbrmsk, port->Untagmsk);
    }
    else
        DBG_INFO(KERN_ERR "vlan get port failed(0x%x): vid=%d\n", ret, port->vid);

    return RET_OK;
}

int ip175d_vlan_entry_set(unsigned int vlanIndex, unsigned int Vid, unsigned int Mbrmsk, unsigned int Untamsk, unsigned int fid, struct net_device *dev)
{
    unsigned int regNum;
    int regVal, old_regVal;

    if ((vlanIndex > 15) || (Vid > IP175d_MAX_VID))
        return -1;

    /* set vlan ID, Fid */
    regNum = 14 + vlanIndex;
    regVal = (Vid & IP175d_MAX_VID) | ((fid & 0xf) << 12);
    mdiobus_write(dev->phydev->mdio.bus, 22, regNum, regVal);

    if(vlanIndex % 2 == 0)//[0:5]
    {
        /* set vlan menbership */
        old_regVal = mdiobus_read(dev->phydev->mdio.bus, 23, vlanIndex/2);
        old_regVal |= 0x3F;
        regVal = (Mbrmsk | (~IP175D_MAX_PORTMASK)) & old_regVal;
        mdiobus_write(dev->phydev->mdio.bus, 23, vlanIndex/2, regVal);
        //DBG_INFO("Mbrmsk: 0x%x\n",regVal);

        /* set vlan untag mask */
        old_regVal = mdiobus_read(dev->phydev->mdio.bus, 23, 8+vlanIndex/2);
        old_regVal |= 0x3F;
        regVal = (Untamsk | (~IP175D_MAX_PORTMASK)) & old_regVal;
        mdiobus_write(dev->phydev->mdio.bus, 23, 8+vlanIndex/2, regVal);
        //DBG_INFO("Untamsk : 0x%x\n",regVal);
    }
    else{// [13:8]
        /* set vlan menbership */
        old_regVal = mdiobus_read(dev->phydev->mdio.bus, 23, vlanIndex/2);
        old_regVal &= 0x3F;
        regVal = ((Mbrmsk & IP175D_MAX_PORTMASK) << 8) | old_regVal;
        mdiobus_write(dev->phydev->mdio.bus, 23, vlanIndex/2, regVal);
        //DBG_INFO("Mbrmsk : 0x%x\n",regVal);
		
        /* set vlan untag mask */
        old_regVal = mdiobus_read(dev->phydev->mdio.bus, 23, 8+vlanIndex/2);
        old_regVal &= 0x3F; 
        regVal = ((Untamsk & IP175D_MAX_PORTMASK) << 8)| old_regVal;
        mdiobus_write(dev->phydev->mdio.bus, 23, 8+vlanIndex/2, regVal);
        //DBG_INFO("Untamsk : 0x%x\n",regVal);
    }

    return RET_OK;
}

int ip175d_vlan_set(unsigned int vid, ip175d_portmask_t mbrmsk, ip175d_portmask_t untagmsk, unsigned int fid, struct net_device *dev)
{
    int i;
    int fullflag, fill_Val, regval,fill_Val1;

    /* vid 1 ~ 4094 */
    if((vid == 0) || vid > (IP175d_MAX_VID -1)){
        printk(KERN_ERR "port VID invalid\n");
        return -1;
    }

    fullflag = FALSE;
    /* cmp table vid and port vid ; vlan full ?  */
    for(i = 0; i < 16; i++)
    {
        /* vlan 15~0  VLAN_VALID */
        regval = mdiobus_read(dev->phydev->mdio.bus, 22, 10);
        //DBG_INFO("vlan original regValue = 0x%d\n",regval);
        fill_Val = regval & (1 << i);
        //DBG_INFO(" Entry VLAN_%d = %d\n",i,fill_Val);
        if(fill_Val)
        {
            fullflag = TRUE;
            continue;
        }
        else{
            /* add vlan */
            ip175d_vlan_entry_set(i, vid, mbrmsk.bits[0], untagmsk.bits[0], 0, dev);
            /*  set  Entry VLAN_VALID 1 */
            fill_Val1 = regval | (1 << i);
            mdiobus_write(dev->phydev->mdio.bus, 22, 10, fill_Val1);
            fullflag = FALSE;
            break;
        }

    }

    if(fullflag == TRUE)
    {
        printk("vlan fulll,please remove vlan or uninit\n");
        return RET_ERROR;
    }

    return RET_OK;

}

int ip175_add_vlan_port(struct net_device *dev, struct ethtool_vlan_port *port)
{
    ip175d_portmask_t mbrmsk, untagmsk;
    int ret;
    unsigned int fid = 0;

    mbrmsk.bits[0] = port->Mbrmsk;
    untagmsk.bits[0] = port->Untagmsk;

    ret = ip175d_vlan_set((unsigned int)port->vid, mbrmsk, untagmsk, fid, dev);
    if(ret == RET_OK)
    {
        DBG_INFO("vlan add port succeed: vid=%d, mbr=%x, Add_tag=%x\n\n",
            port->vid, mbrmsk.bits[0], untagmsk.bits[0]);
    }
    else{
        DBG_INFO(KERN_ERR "vlan add port failed(0x%x): vid=%d, mbr=%x, Add_tag=%x\n\n",
                ret, port->vid, mbrmsk.bits[0], mbrmsk.bits[0]);		
    }

    return RET_OK;
}

int ip175_remove_vlan_port(struct net_device *dev, struct ethtool_vlan_port *port)
{
    int i, regVal;
    ip175d_portmask_t mbrmsk, untagmsk;
    unsigned int vid_val, mbrmsk_val, untagmsk_val, fid_val;
    unsigned int fid = 0;

    mbrmsk.bits[0] = 0;
    untagmsk.bits[0] = 0;
	
    /* get vid, mbrmsk, untagmask, fid  & clear vlan */
    for (i = 15; i >= 0; i--)
    {
        ip175d_vlan_entry_get(i, &vid_val, &mbrmsk_val, &untagmsk_val, &fid_val, dev);
        if(vid_val == port->vid)
        {
            mbrmsk_val = 0x3f;
            untagmsk_val = untagmsk.bits[0];
            /*set vlan id=0, mbr=0x3f, untagmsk=0, fid=0*/
            ip175d_vlan_entry_set(i, 0, mbrmsk_val, untagmsk_val, fid, dev);
            regVal = mdiobus_read(dev->phydev->mdio.bus, 22, 10);
            regVal = regVal & (~(1 << i));
            mdiobus_write(dev->phydev->mdio.bus, 22, 10, regVal);//set 0
            return RET_OK;
        }
    }

    return RET_OK;   
}

int ip175d_remove_tag_set(unsigned int vlanIndex, unsigned int port, struct net_device *dev, unsigned int Flag)
{
    int old_Val, regVal;

	/* Different Vid  Set Remove-tag */
    if(DIFFERENT_PORT_TAG_FLAG == Flag)
    {
        if(vlanIndex % 2 == 0)//low bit [0:5]
        {
            /* get old reg-Val */
            old_Val = mdiobus_read(dev->phydev->mdio.bus, 23, 16+vlanIndex/2);
            //DBG_INFO("[ %d ]  Old remove-tag Reg_Val ：0x%x\n",__LINE__,old_Val);
            old_Val |= 0x3F;
            regVal = ((1 << port) | (~IP175D_MAX_PORTMASK)) & old_Val; 
            //DBG_INFO("[ %d ]  new remove-tag Reg_Val ：0x%x\n",__LINE__,regVal);
            // set remove tag
            mdiobus_write(dev->phydev->mdio.bus, 23, 16+vlanIndex/2, regVal);
        }
        else{//hight bit[13:8]

            /* get old reg-Val */
            old_Val = mdiobus_read(dev->phydev->mdio.bus, 23, 16+vlanIndex/2);
            old_Val &= 0x3F;
            regVal = (((1 << port) & IP175D_MAX_PORTMASK) << 8) | old_Val; 
            //DBG_INFO("[ %d ] new remove-tag Reg_Val ：0x%x\n",__LINE__,regVal);
            /* set remove tag */
            mdiobus_write(dev->phydev->mdio.bus, 23, 16+vlanIndex/2, regVal);
        }
    }
	/* One Port Enabled Set Remove-tag is 0*/
    if(LAN_PORT_TAG_FLAG == Flag)
    {
        if(vlanIndex % 2 == 0){/* [0:5] set 0 */
            /* get old reg-Val */
            old_Val = mdiobus_read(dev->phydev->mdio.bus, 23, 16+vlanIndex/2);
            //DBG_INFO("[ %d ]  Old remove-tag Reg_Val ：0x%x\n",__LINE__,old_Val);
            old_Val |= 0x3F;
            regVal = old_Val & 0xFF00;//0
            //DBG_INFO("[ %d ] new remove-tag Reg_Val ：0x%x\n",__LINE__,regVal);
            /* set remove tag */
            mdiobus_write(dev->phydev->mdio.bus, 23, 16+vlanIndex/2, regVal);
        }
        else{	/* [13:8]  set 0 */
            /* get old reg-Val */
            old_Val = mdiobus_read(dev->phydev->mdio.bus, 23, 16+vlanIndex/2);
            //DBG_INFO("[ %d ]  Old remove-tag Reg_Val ：0x%x\n",__LINE__,old_Val);
            old_Val &= 0x3F3F;
            regVal = old_Val & 0x00FF; //0
            //DBG_INFO("[ %d ] new remove-tag Reg_Val ：0x%x\n",__LINE__,regVal);
            /* set remove tag */
            mdiobus_write(dev->phydev->mdio.bus, 23, 16+vlanIndex/2, regVal);
        }
    }
    /* LAN Port & WAN Port Enabled Set Remove-tag ; fixed VLAN0  */
    if(DOUBLE_EQUAL_PORT_TAG_FLAG == Flag){
        /* get old reg-Val */
        old_Val = mdiobus_read(dev->phydev->mdio.bus, 23, 16+vlanIndex/2);
        regVal = (1 << port) | old_Val;
        /* set remove tag */
        mdiobus_write(dev->phydev->mdio.bus, 23, 16+vlanIndex/2, regVal);
    }

    return RET_OK;
}

int ip175d_vlan_port_remove_tag(unsigned int port, unsigned int vid, struct net_device *dev)
{
    unsigned int vid_val, mbrmsk_val, untagmsk_val, fid_val, Pmbrmsk;
    int i ,regVal, Disable_flag, fill_Val, Enable_bit, Enable_Val;
    for(i = 0; i < 16; i++)
    {
        ip175d_vlan_entry_get(i, &vid_val, &mbrmsk_val, &untagmsk_val, &fid_val, dev);
        Pmbrmsk = mbrmsk_val & 0x23;/*  Port0, Port1, Port5	*/
		/* Wan & Lan Port Enabled; Different Vid  Set Remove-tag */
        if(vid_val == vid && untagmsk_val != 0 && Pmbrmsk != 0x23)
        {
            ip175d_remove_tag_set(i, port, dev, DIFFERENT_PORT_TAG_FLAG);
            break;
        }
		/* Wan & Lan Port Enabled; The same Vid  Set Remove-tag */
        if(vid_val == vid && untagmsk_val != 0 && Pmbrmsk == 0x23){
            ip175d_remove_tag_set(i, port, dev, DOUBLE_EQUAL_PORT_TAG_FLAG);
            break;
        }
		/*  WAN Port Enabled ;  Set Remove-tag ; Port1: untagpackt     */
        if(vid_val == vid  && untagmsk_val == 0 && port == PHY_LAN_PORT)
        {
            //DBG_INFO("WAN Port Enabled-->> vid:0x%x, mbrmsk_val: 0x%x, add-tag: 0x%x , port: %d \n",vid_val,mbrmsk_val,untagmsk_val,port);
            ip175d_remove_tag_set(i, port, dev, LAN_PORT_TAG_FLAG);
            break;
        }
		/* LAN Port Enabled ;  Set Remove-tag ; Fixed Entry_5 & VLAN_5 ; Port5: untagpackt   */
        if(vid_val == vid && untagmsk_val == 0 && port == PHY_CPU_PORT)
        {
            /*1. Set Entry_i Disenable*/
            Disable_flag = (1 << i);
            regVal = mdiobus_read(dev->phydev->mdio.bus, 22, 10);
            fill_Val = regVal & (~Disable_flag);
            mdiobus_write(dev->phydev->mdio.bus, 22, 10, fill_Val);
            /*2. clear vlan vid=0, membermask=0x3f/0, untagmask, remove-tag*/
            ip175d_vlan_entry_set(i, 0, 0x3f, 0, 0, dev);

            /*3. Set Entry_5 enable  fixed VLAN5 */
            Enable_bit = 1 << port;
            regVal = mdiobus_read(dev->phydev->mdio.bus, 22, 10);
            Enable_Val = regVal | Enable_bit;
            mdiobus_write(dev->phydev->mdio.bus, 22, 10, Enable_Val);
            /*4. Set VLAN_5 vid, membermask, add-tagmask*/
            ip175d_vlan_entry_set(port, vid_val, mbrmsk_val, untagmsk_val, fid_val, dev);
            /*5. Set remove-tag*/
            ip175d_remove_tag_set(i, port, dev, LAN_PORT_TAG_FLAG);
            break;
        }
    }
    return RET_OK;
}

#if 0
int ip175d_remove_tag_get(unsigned int vlanIndex, unsigned int port, struct net_device *dev)
{
    int regVal;
    //unsigned int regNum;

    if(vlanIndex % 2 == 0)//low bit [0:5]
    {
        // get old reg-Val
        regVal = mdiobus_read(dev->phydev->mdio.bus, 23, 16+vlanIndex/2);
        DBG_INFO("VLAN_%d Remove-tag：0x%x\n",vlanIndex,regVal);
    }
    else{//hight bit[13:8]
	
        // get old reg-Val
        regVal = mdiobus_read(dev->phydev->mdio.bus, 23, 16+vlanIndex/2);
        DBG_INFO("VLAN_%d Remove-tag：0x%x\n",vlanIndex,regVal);
    }
    return RET_OK;
}
#endif

int ip175d_vlan_port_remove_tag_get(unsigned int port, unsigned int vid, struct net_device *dev)
{
//    unsigned int vid_val, mbrmsk_val, untagmsk_val, fid_val;
    int i;
    int regVal;
	
    for(i = 0; i < 8; i++)
    {
    /*
        ip175d_vlan_entry_get(i, &vid_val, &mbrmsk_val, &untagmsk_val, &fid_val, dev);
        if(vid_val == vid)
        {
            //根据端口移位设置remove tag
            ip175d_remove_tag_get(i, port, dev);
            break;
        }
    */
        regVal = mdiobus_read(dev->phydev->mdio.bus, 23, 16+i);
        DBG_INFO("VLAN_%d Remove-tag：0x%x\n",i,regVal);
    }
    return RET_OK;

}

int ip175d_vlan_portPvid_set(unsigned int port, unsigned int vid, struct net_device *dev)
{
    unsigned int vid_val, mbrmsk_val, untagmsk_val, fid, pvid, Pmbrmsk;
    int i, fill_Val, regval;
    unsigned int regNum;

    if (port > 5)
        return RET_ERROR;

    regNum = 4 + port;

    /*pvid_0 ~ pvid_5 <==> vid_0 ~ vid_5
    search vlan table ;  get vid & set pvid */
    for(i = 0; i < 6; i++)
    {
        //vlan 15~0 vlan_valid 	
        regval = mdiobus_read(dev->phydev->mdio.bus, 22, 10);
        fill_Val = regval & (1 << i);
        if(fill_Val)
        { 
            ip175d_vlan_entry_get(i, &vid_val, &mbrmsk_val, &untagmsk_val, &fid, dev);
            Pmbrmsk =  (mbrmsk_val >> port) & 0x1;
            /* Set pvid = vid  */
            if(vid_val == vid && Pmbrmsk)
            {
                pvid = vid_val;
                mdiobus_write(dev->phydev->mdio.bus, 22, regNum, pvid);//pvid
            }
            continue;
        }
    }
	
    return RET_OK;
}

int ip175_set_untagged_vlan_port(struct net_device *dev, struct ethtool_untagged_vlan_port * uport)
{
    int ret;

    /* Set pvid = vid */
    ip175d_vlan_portPvid_set((unsigned int) uport->port, uport->vid, dev);

    /* Set remove tag  by Port num*/
    ret =  ip175d_vlan_port_remove_tag((unsigned int) uport->port, uport->vid, dev);
    if (ret != RET_OK) {
        DBG_INFO(KERN_ERR "vlan set port untagged failed(0x%x): port=%d\n\n",
            ret, uport->port);
    }	

    return ret;
}

int ip175_get_untagged_vlan_port(struct net_device *dev, struct ethtool_untagged_vlan_port *uport)
{
    int ret;
    unsigned int regNum;
    int regVal;

    /* get pvid  port 0 ~ port 5 */
    if (uport->port > MAX_PORTS-1)
        return RET_ERROR;
    regNum = 4 + uport->port;
    regVal = mdiobus_read(dev->phydev->mdio.bus, 22, regNum);
    DBG_INFO("Port %d pVid: 0x%x\n",uport->port,regVal);

    /* get remove-tag */
    ret = ip175d_vlan_port_remove_tag_get((unsigned int) uport->port, uport->vid, dev);
    if(ret != RET_OK) {
        DBG_INFO(KERN_ERR "vlan get port untagged failed(0x%x): port=%d, vid=%d \n",
            ret, uport->port,uport->vid);
    }

    return ret;

}

int ip175d_rew_vlan_pri_set(unsigned int vlanIndex, unsigned int port, unsigned int priority, struct net_device *dev)
{
    unsigned int old_Val, regVal;

    if(vlanIndex % 2 == 0)//low bit [7:5]
    {
        // get old reg-Val
        old_Val = mdiobus_read(dev->phydev->mdio.bus, 23, 24+vlanIndex/2);
        old_Val &= (~0xe0);//000x xxxx
        regVal = (priority << 5) | old_Val; //xxx << 5
        // set priority
        mdiobus_write(dev->phydev->mdio.bus, 23, 24+vlanIndex/2, regVal);
    }
    else{// [15:13]
        // get old reg-Val
        old_Val = mdiobus_read(dev->phydev->mdio.bus, 23, 24+vlanIndex/2);
        old_Val &= (~0xe000);//000x xxxx xxxx xxxx
        regVal = (priority << 13) | old_Val;
        // set remove tag
        mdiobus_write(dev->phydev->mdio.bus, 23, 24+vlanIndex/2, regVal);
    }

    return RET_OK;
}

int ip175d_rew_vlan_pri(unsigned int port, unsigned int priority, struct net_device *dev)
{
    unsigned int regVal, regBit, regNum, prioVal;
    unsigned int vid_val, mbrmsk_val, untagmsk_val, fid_val, Pmbrmsk, pvid;
    int i;
	
    /* port pvid */
    regNum = 4+port;
    pvid = mdiobus_read(dev->phydev->mdio.bus, 22, regNum);
    //DBG_INFO("Port PVID:%d\n",pvid);
	
    for(i = 0; i < 16; i++)
    {
        /* ensure VLAN_VALID value  */
        regVal = mdiobus_read(dev->phydev->mdio.bus, 22, 10);
        //DBG_INFO("VLAN_VALID ：0x%x\n",regVal);
        regBit = (regVal >> i) & 0x1;
        //DBG_INFO("VLAN_%d, VALID ：0x%x\n",i,regBit);
        if(regBit)
        {
            /* search vlan  valid vlan */
            ip175d_vlan_entry_get(i, &vid_val, &mbrmsk_val, &untagmsk_val, &fid_val, dev);
            Pmbrmsk = mbrmsk_val & 0x23;//Port0, Port1, Port5
            /*  vid_val == pvid ; That means the other VLANs are zero */
            if(vid_val == pvid)
            {
                //DBG_INFO("Enable Wan & Lan Port , Different ID\n");
            /*  Entry i of re-write vlan priority field is enable 	  */
                prioVal = mdiobus_read(dev->phydev->mdio.bus, 22, 13);
                prioVal = (1 << i) | prioVal;
                //DBG_INFO("Enable VLAN_%d Priority: 0x%x\n",i,prioVal);
                mdiobus_write(dev->phydev->mdio.bus, 22, 13, 0x0003);
                /* set priority */
                ip175d_rew_vlan_pri_set(i, port, priority, dev);
                break;
            }
        }	
    }

    return RET_OK;
}

int ip175_set_untagged_vlan_port_priority(struct net_device *dev, struct ethtool_untagged_port_priority *ppriority)
{
    int ret;
	
    if(ppriority->port > (MAX_PORTS -1))
        return RET_ERROR;
    if(ppriority->priority > 7)
        return RET_ERROR;

    ret = ip175d_rew_vlan_pri(ppriority->port, ppriority->priority, dev);
    if(ret == RET_OK)
        DBG_INFO(" vlan priority set succeed:  port=%d, priority=0x%x \n ",ppriority->port,ppriority->priority);

    return RET_OK;	
}

int ip175_get_untagged_vlan_port_priority(struct net_device * dev, struct ethtool_untagged_port_priority *ppriority)
{
    int i;
    unsigned int Priority;

    if (ppriority->port > MAX_PORTS-1)
        return RET_ERROR;

    for(i = 0; i < 8; i++)
    {
        Priority = mdiobus_read(dev->phydev->mdio.bus, 23, 24+i);
        DBG_INFO("[7:5] vlan get port untagged priority succeed: 0x%x\n",Priority);
    }

    return RET_OK;
}

#if 0
int dscp_write_regval(unsigned int dscp, unsigned int priority, struct net_device *dev, unsigned int regNum)
{
    unsigned int old_regVal, dscpNum, dscpBit, tmp, rew_priority;

    old_regVal = mdiobus_read(dev->phydev->mdio.bus, 25, 3+regNum);
    dscpNum = (dscp % 8);
    dscpBit = 2*dscpNum;
    old_regVal = (~(0x3 << dscpBit)) & old_regVal;
    tmp = priority << dscpBit;
    rew_priority = tmp | old_regVal;
    mdiobus_write(dev->phydev->mdio.bus, 25, 3+regNum, rew_priority);

    return RET_OK;
}

int ip175d_qos_dscpPri_set(unsigned int dscp, unsigned int priority, struct net_device *dev)
{
    if(dscp < 0 || priority >= 4)
        return RET_ERROR;

    if(dscp <= 0x7){
        dscp_write_regval(dscp, priority, dev, 0);
    }
    else if(dscp >= 0x8 && dscp <= 0xF){
        dscp_write_regval(dscp, priority, dev, 1);
    }
    else if(dscp >= 0x10 && dscp <= 0x17){
        dscp_write_regval(dscp, priority, dev, 2);
    }
    else if(dscp >= 0x18 && dscp <= 0x1F){
        dscp_write_regval(dscp, priority, dev, 3);
    }
    else if(dscp >= 0x20 && dscp <= 0x27){
        dscp_write_regval(dscp, priority, dev, 4);
    }
    else if(dscp >= 0x28 && dscp <= 0x2F){
        dscp_write_regval(dscp, priority, dev, 5);
    }
    else if(dscp >= 0x30 && dscp <= 0x37){
        dscp_write_regval(dscp, priority, dev, 6);
    }
    else if(dscp >= 0x38 && dscp <= 0x3F){
        dscp_write_regval(dscp, priority, dev, 7);
    }
    else{
        DBG_INFO("Dscp Out Of Range!\n");
        return RET_ERROR;
    }

    return RET_OK;
}

int ip175_set_dscp_priority(struct net_device *dev, struct ethtool_dscp_priority *priority)
{
    int ret;
    if(priority->prior >= 4){
        DBG_INFO(KERN_ERR "Invalid Priority\n");
        return RET_OK;
    }
    if(priority->dscp > 0x3F ){
        DBG_INFO(KERN_ERR "Invalid Dscp\n");
        return RET_OK;
    }

    ret = ip175d_qos_dscpPri_set((unsigned int) priority->dscp, (unsigned int) priority->prior, dev);
    if(ret != RET_OK) {
        DBG_INFO(KERN_ERR "qos dscp priority set failed(0x%x): dscp=%d, prior=%d\n",
            ret, priority->dscp, priority->prior);
    }
    else {
        DBG_INFO("qos dscp priority set succeed: dscp=%d, prior=%d\n",
            priority->dscp, priority->prior);
    }

    return ret;
}


int ip175_get_dscp_priority(struct net_device *dev, struct ethtool_dscp_priority *priority)
{
    int i;
    unsigned int regVal;

    for(i = 0; i < 8; i++)
    {
        regVal = mdiobus_read(dev->phydev->mdio.bus, 25, 3+i);
        DBG_INFO("DSCP priority Map : 0x%x\n",regVal);
    }

    return RET_OK;
}
#endif

int ip175_uninit_vlan(struct net_device *dev, struct ethtool_vlan_port *port)
{
    unsigned int vid_val, mbrmsk_val, untagmsk_val, fid_val;
    int i, j;
	
    port->Mbrmsk = 0x3f;
    port->Untagmsk = 0;
    /* VLAN Classification Default */
    mdiobus_write(dev->phydev->mdio.bus, 22, 0, 0x0000);
    /* PVID Default */
    for(i = 0; i < 6; i++)
        mdiobus_write(dev->phydev->mdio.bus, 22, 4+i, 0x0001);
    /* VLAN_VALID set 0 Default */
    mdiobus_write(dev->phydev->mdio.bus, 22, 10, 0x0000);
    /* VLAN_PRI ( REW_VLAN_PRI_EN ) set 0 Default */
    mdiobus_write(dev->phydev->mdio.bus, 22, 13, 0x0000);
    for(i = 0; i < 8; i++)
    {	
        //mdiobus_write(dev->phydev->mdio.bus, 23, i, 0x3f3f);//VLAN Member Default
        //mdiobus_write(dev->phydev->mdio.bus, 23, 8+i, 0x0000);//VLAN Add Tag Default
        mdiobus_write(dev->phydev->mdio.bus, 23, 16+i, 0x0000);//VLAN Remove Tag  Default
        mdiobus_write(dev->phydev->mdio.bus, 23, 24+i, 0x0000);//VLAN Miscellaneous Register Default (VLAN PRIORITY)
    }
    for(j = 0; j < 16; j++)
    {
        ip175d_vlan_entry_get(j, &vid_val, &mbrmsk_val, &untagmsk_val, &fid_val, dev);
        if(mbrmsk_val !=0)
        {
            //VLAN ID Default /VLAN Member Default / VLAN Add Tag Default
            ip175d_vlan_entry_set(j, 0, port->Mbrmsk, port->Untagmsk, 0, dev);
            continue;
        }
    }

    return RET_OK;
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

