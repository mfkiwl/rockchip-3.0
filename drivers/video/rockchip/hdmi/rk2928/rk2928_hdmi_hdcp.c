#include <linux/delay.h>
#include "rk2928_hdmi.h"
#include "rk2928_hdmi_hw.h"
#include "rk2928_hdcp.h"

#define HDCPWrReg	HDMIWrReg
#define HDCPRdReg	HDMIRdReg
#define HDCPMskReg(temp, addr, msk, val)	\
	temp = HDCPRdReg(addr) & (0xFF - (msk)) ; \
	HDCPWrReg(addr, temp | ( (val) & (msk) )); 
	
void rk2928_hdcp_disable(struct hdcp *hdcp)
{
	struct rk2928_hdmi *rk2928_hdmi = hdcp->hdmi->property->priv;
	char temp;
	
	// Diable HDCP Interrupt
	HDCPWrReg(HDCP_INT_MASK1, 0x00);
	// Stop and Reset HDCP
	HDCPMskReg(temp, HDCP_CTRL1, m_ENCRYPT_ENABLE | m_AUTH_STOP | m_HDCP_RESET, 
		v_ENCRYPT_ENABLE(0) | v_AUTH_STOP(1) | v_HDCP_RESET(1) )
}

int	rk2928_hdcp_load_key2mem(struct hdcp *hdcp, struct hdcp_keys *key)
{
	struct rk2928_hdmi *rk2928_hdmi = hdcp->hdmi->property->priv;
	int i;
	HDCPDBG("HDCP: rk2928_hdcp_load_key2mem start");
	// Write 40 private key
	for(i = 0; i < HDCP_PRIVATE_KEY_SIZE; i++)
		HDCPWrReg(HDCP_KEY_FIFO, key->DeviceKey[i]);
	
	// Write 1st aksv
	for(i = 0; i < 5; i++)
		HDCPWrReg(HDCP_KEY_FIFO, key->KSV[i]);
		
	// Write 2nd aksv
	for(i = 0; i < 5; i++)
		HDCPWrReg(HDCP_KEY_FIFO, key->KSV[i]);
	HDCPDBG("HDCP: rk2928_hdcp_load_key2mem end");
	return HDCP_OK;
}

int	rk2928_hdcp_start_authentication(struct hdcp *hdcp)
{
	struct rk2928_hdmi *rk2928_hdmi = hdcp->hdmi->property->priv;
	char temp;
	int retry = 0;

	if(hdcp->keys == NULL) {
		printk(KERN_ERR "HDCP: key is not loaded\n");
		return HDCP_KEY_ERR;
	}
	
	// Select TMDS CLK to configure regs
	HDCPMskReg(temp, SYS_CTRL, m_REG_CLK_SOURCE, v_REG_CLK_SOURCE_TMDS);
	
	temp = HDCPRdReg(HDCP_KEY_STATUS);
	while( ( temp & m_KEY_READY) == 0 ) {
		if(retry > 10) {
			printk(KERN_ERR "HDCP: loaded key error\n");
			return HDCP_KEY_ERR;
		}
		rk2928_hdcp_load_key2mem(hdcp, hdcp->keys);
		msleep(1);
		temp = HDCPRdReg(HDCP_KEY_STATUS);
	}
	
	// Config DDC bus clock: ddc_clk = reg_clk/4*(reg 0x4c 0x4b)
	HDCPDBG("TMDS frequency %d", rk2928_hdmi->tmdsclk);
	retry = rk2928_hdmi->tmdsclk/(HDCP_DDC_CLK*4);
	HDCPWrReg(DDC_CLK_L, retry & 0xFF);
	HDCPWrReg(DDC_CLK_H, (retry >> 8) & 0xFF);
	
	HDCPWrReg(HDCP_CTRL2, 0x00);
	
	//Enable interrupt
	HDCPWrReg(HDCP_INT_MASK1, m_INT_HDCP_ERR | m_INT_BKSV_READY | m_INT_BKSV_UPDATE | m_INT_AUTH_SUCCESS | m_INT_AUTH_READY);
//	HDCPWrReg(HDCP_INT_MASK2, 0xFF);
	//Start authentication
	HDCPMskReg(temp, HDCP_CTRL1, m_AUTH_START | m_ENCRYPT_ENABLE | m_ADVANED_ENABLE, v_AUTH_START(1) | v_ENCRYPT_ENABLE(1) | v_ADVANED_ENABLE(0));
	
	return HDCP_OK;
}

int	rk2928_hdcp_check_bksv(struct hdcp *hdcp)
{
	int i, j;
	char temp = 0, bksv[5];
	char *invalidkey;
	struct rk2928_hdmi *rk2928_hdmi = hdcp->hdmi->property->priv;
	
	for(i = 0; i < 5; i++) {
		bksv[i] = HDCPRdReg(HDCP_KSV_BYTE0 + (4 - i)) & 0xFF;
	}
	HDCPDBG("bksv is 0x%02x%02x%02x%02x%02x", bksv[0], bksv[1], bksv[2], bksv[3], bksv[4]);
	
	for (i = 0; i < 5; i++)
	{
    	for (j = 0; j < 8; j++)
    	{
    		if (bksv[i] & 0x01)
    		{
        		temp++;
    		}
    		bksv[i] >>= 1;
    	}
 	}
 	if (temp != 20)
    	return HDCP_KSV_ERR;
	
	for(i = 0; i < hdcp->invalidkey; i++)
	{
		invalidkey = hdcp->invalidkeys + i *5;
		if(memcmp(bksv, invalidkey, 5) == 0) {
			printk(KERN_ERR "HDCP: BKSV was revocated!!!\n");
			HDCPMskReg(temp, HDCP_CTRL1, m_BKSV_INVALID | m_ENCRYPT_ENABLE, v_BKSV_INVALID(1) | v_ENCRYPT_ENABLE(1));
			return HDCP_KSV_ERR;
		}
	}
	HDCPMskReg(temp, HDCP_CTRL1, m_BKSV_VALID | m_ENCRYPT_ENABLE, v_BKSV_VALID(1) | v_ENCRYPT_ENABLE(1));
	return HDCP_OK;
}

void rk2928_hdcp_interrupt(struct hdcp *hdcp, char *status1, char *status2)
{
	struct rk2928_hdmi *rk2928_hdmi = hdcp->hdmi->property->priv;
	
	char interrupt1 = HDCPRdReg(HDCP_INT_STATUS1);
	char interrupt2 = HDCPRdReg(HDCP_INT_STATUS2);
	if(interrupt1) {
		HDCPWrReg(HDCP_INT_STATUS1, interrupt1);
		if(interrupt1 & m_INT_HDCP_ERR)
			printk(KERN_INFO "HDCP: Error 0x%02x\n", HDCPRdReg(HDCP_ERROR));
	}
	if(interrupt2)
		HDCPWrReg(HDCP_INT_STATUS2, interrupt2);
	
	*status1 = interrupt1;
	*status2 = interrupt2;
}