/*
    voodoo3.c - Part of lm_sensors, Linux kernel modules for hardware
              monitoring
    Copyright (c) 1998, 1999  Frodo Looijaard <frodol@dds.nl>,
    Philip Edelbrock <phil@netroedge.com> and 
    Ralph Metzler <rjkm@thp.uni-koeln.de>
    
    Based on code written by Ralph Metzler <rjkm@thp.uni-koeln.de> and
    Simon Vogl

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#define DEBUG 1

/* This interfaces to the I2C bus of the Voodoo3 to gain access to
    the BT869 and possibly other I2C devices. */

#include <linux/module.h>
#include <linux/pci.h>
#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include "smbus.h"
#include "version.h"
#include "compat.h"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,1,54))
#include <linux/bios32.h>
#endif

/* 3DFX defines */
#ifndef PCI_VENDOR_ID_3DFX
#define PCI_VENDOR_ID_3DFX 0x121a
#endif
#ifndef PCI_DEVICE_ID_3DFX_VOODOO3
#define PCI_DEVICE_ID_3DFX_VOODOO3 0x05
#endif

/* insmod parameters */

static int voodoo3_init(void);
static int voodoo3_cleanup(void);
static int voodoo3_setup(void);
static s32 voodoo3_access(u8 addr, char read_write,
                        u8 command, int size, union smbus_data * data);

#ifdef MODULE
extern int init_module(void);
extern int cleanup_module(void);
#endif /* MODULE */

static struct smbus_adapter voodoo3_adapter;
static int voodoo3_initialized;
static unsigned short voodoo3_smba = 0;
static unsigned int state=0xcf980020;
static unsigned char *mem;
static int v3_num;

inline outlong(int off,unsigned int dat)
{
        *((unsigned int*)(mem+off))=dat;
}


inline unsigned int readlong(int off)
{
        return *((unsigned int*)(mem+off));
}

inline out(void)
{
        outlong(0x78,state);
        udelay(10);
}

inline void dat(int data)
{
  state&=~(1<<25);
  if (data)
    state|=(1<<25);
}

inline void clkon(void)
{
  state|=(1<<24);
}

inline void clkoff(void)
{
  state&=~(1<<24);
}

inline int rdat(void)
{
        dat(1);
        out();
        return((readlong(0x78)&(1<<27) )!=0 );
}

/* Changing the Data line while clock is 'on' (high) is a
   no-no, except for a start or stop.  */
   
void Voodoo3_I2CStart(void)
{
  clkon(); /* in theory, clk is already on */
  out();
  dat(0);
  out();
  clkoff();
  out();
  clkon();
}

void Voodoo3_I2CStop(void)
{
  dat(0);
  out();
  clkon();
  out();
  dat(1);
  out();
  clkoff();
  out();
  clkon();
  out();
}

int Voodoo3_I2CAck()
{
int ack;

  dat(1); /* put data line in tristate/hi */
  clkon();
  out();
  ack=rdat();
  clkoff();
  out();
  return ack;

}

int Voodoo3_I2CReadByte()
{
  int i,temp;
  unsigned char data=0;

  clkoff();
  out();
  for (i=7; i>=0; i--) {
    out();
    clkon();
    data|=(rdat()<<i);
    out();
    clkoff();
  }
  temp=Voodoo3_I2CAck();
#ifdef DEBUG
  printk("i2c-voodoo3: Readbyte ack -->0x%X, read result -->0x%X\n",temp,data);
#endif
  if (! temp)
   return data;
  else {
   return -1;
  }
}

int Voodoo3_I2CSendByte(unsigned char data)
{
  int i,temp;

  clkoff();
  out();
  for (i=7; i>=0; i--) {
    dat(temp=data&(1<<i));
    out();
    clkon();
    out();
    dat(temp);
    clkoff();
    out();
  }
  temp=Voodoo3_I2CAck();
#ifdef DEBUG
  printk("i2c-voodoo3: Writebyte ack -->0x%X\n",temp);
#endif
  return temp;
}


int Voodoo3_BusCheck(void) {
/* Check the bus to see if it is in use */

  if (! rdat()) {
    printk("i2c-voodoo3: I2C bus in use or hung!  Try again later.\n");
    return 1;
  }
  return 0;
}

/* Note, this is actually a byte read and not a byte_data read */
/* I.e., the command value is dumped.                          */
int Voodoo3_I2CRead(int addr,int command)
{
        int dat=0;

        Voodoo3_I2CStart();
        if (Voodoo3_I2CSendByte(addr)) { 
#ifdef DEBUG
	  printk("i2c-voodoo3: No Ack on addr WriteByte to addr 0x%X\n",addr);
#endif
	  dat=-1;
	  goto ENDREAD; }
        dat=Voodoo3_I2CReadByte();
ENDREAD: Voodoo3_I2CStop();
#ifdef DEBUG
	printk("i2c-voodoo3: Byte read at addr:0x%X (command:0x%X) result:0x%X\n",addr,command,dat);
#endif
        return dat;
}

int Voodoo3_I2CWrite(int addr,int command,int data)
{
int result=0;

        Voodoo3_I2CStart();
        if (Voodoo3_I2CSendByte(addr)) { 
#ifdef DEBUG
	  printk("i2c-voodoo3: No Ack on addr WriteByte to addr 0x%X\n",addr);
#endif
	  result=-1;
	  goto ENDWRITE; }
        if (Voodoo3_I2CSendByte(command)) {
#ifdef DEBUG
	  printk("i2c-voodoo3: No Ack on command WriteByte to addr 0x%X\n",addr);
#endif
	  result=-1;
	  goto ENDWRITE; }
        if (Voodoo3_I2CSendByte(data)) {
#ifdef DEBUG
	  printk("i2c-voodoo3: No Ack on data WriteByte to addr 0x%X\n",addr);
#endif
 	}
	result=-1;
ENDWRITE: Voodoo3_I2CStop();
#ifdef DEBUG
	printk("i2c-voodoo3: Byte write at addr:0x%X command:0x%X data:0x%X\n",addr,command,data);
#endif
	return result;
}

void config_v3(struct pci_dev *dev, int num)
{
        unsigned int cadr;

        /* map Voodoo3 memory */
        cadr=dev->base_address[0];
        cadr&=PCI_BASE_ADDRESS_MEM_MASK;
        mem=ioremap(cadr, 0x1000);
        
        /* Enable TV out mode, Voodoo3_I2C bus, etc. */
        *((unsigned int *)(mem+0x70))=0x8160;
        *((unsigned int *)(mem+0x78))=0xcf980020;
	printk("i2c-voodoo3: Using Banshee/Voodoo3 at 0x%X\n",mem);
}



/* Detect whether a Voodoo3 can be found, and initialize it, where necessary.
   Note the differences between kernels with the old PCI BIOS interface and
   newer kernels with the real PCI interface. In compat.h some things are
   defined to make the transition easier. */
static int voodoo3_setup(void)
{
        struct pci_dev *dev = pci_devices;
        int result=0;
	int flag=0;

        v3_num=0;

        while (dev)
        {
                if (dev->vendor == PCI_VENDOR_ID_3DFX)
                        if (dev->device == PCI_DEVICE_ID_3DFX_VOODOO3) {
			  if (!flag) {
                                config_v3(dev,v3_num++);
			  } else { v3_num++; }
			  flag=1;
			}
                if (result)
                        return result;
                dev = dev->next;
        }
        if(v3_num > 0) {
                printk(KERN_INFO "i2c-voodoo3: %d Banshee/Voodoo3(s) found.\n", v3_num);
		return 0;
        } else {
                printk(KERN_INFO "v3tv: No Voodoo3 found.\n");
        	return -ENODEV;
	}
}


/* Return -1 on error. See smbus.h for more information */
s32 voodoo3_access(u8 addr, char read_write,
                 u8 command, int size, union smbus_data * data)
{
int temp;

  if (Voodoo3_BusCheck()) return -1;

  if ((size == SMBUS_BYTE_DATA) || (size == SMBUS_BYTE)) {
        addr=((addr & 0x7f) << 1) | (read_write & 0x01);
  	if (read_write == SMBUS_READ) {
    	  temp=Voodoo3_I2CRead(addr,command);
	  if (temp < 0) {
#ifdef DEBUG
		printk("i2c-voodoo3: no device at 0x%X\n",addr);
#endif
		return -1;
	  }
	  data->byte=temp; 
#ifdef DEBUG
		printk("i2c-voodoo3: access returning 0x%X\n",data->byte);
#endif
	} else {
	  return Voodoo3_I2CWrite(addr,command,data->byte);
	}
  	return 0;
  } else { return -1; } /* The rest isn't implemented yet */
}


int voodoo3_init(void)
{
  int res;
  printk("i2c-voodoo3.o version %s (%s)\n",LM_VERSION,LM_DATE);
#ifdef DEBUG
/* PE- It might be good to make this a permanent part of the code! */
  if (voodoo3_initialized) {
    printk("i2c-voodoo3.o: Oops, voodoo3_init called a second time!\n");
    return -EBUSY;
  }
#endif
  voodoo3_initialized = 0;
  if ((res = voodoo3_setup())) {
    printk("i2c-voodoo3.o: Voodoo3 not detected, module not inserted.\n");
    voodoo3_cleanup();
    return res;
  }
  voodoo3_initialized ++;
  sprintf(voodoo3_adapter.name,"SMBus Voodoo3 adapter at %04x",voodoo3_smba);
  voodoo3_adapter.id = ALGO_SMBUS | SMBUS_VOODOO3;
  voodoo3_adapter.algo = &smbus_algorithm;
  voodoo3_adapter.smbus_access = &voodoo3_access;
  if ((res = smbus_add_adapter(&voodoo3_adapter))) {
    printk("i2c-voodoo3.o: Adapter registration failed, module not inserted.\n");
    voodoo3_cleanup();
    return res;
  }
  voodoo3_initialized++;
  printk("i2c-voodoo3.o: Voodoo3 I2C bus detected and initialized\n");
  return 0;
}

int voodoo3_cleanup(void)
{
  int res;
  
  iounmap(mem);
  if (voodoo3_initialized >= 2)
  {
    if ((res = smbus_del_adapter(&voodoo3_adapter))) {
      printk("i2c-voodoo3.o: smbus_del_adapter failed, module not removed\n");
      return res;
    } else
      voodoo3_initialized--;
  }
  if (voodoo3_initialized >= 1) {
    voodoo3_initialized--;
  }
  return 0;
}

#ifdef MODULE

MODULE_AUTHOR("Frodo Looijaard <frodol@dds.nl> and Philip Edelbrock <phil@netroedge.com> and Ralph Metzler <rjkm@thp.uni-koeln.de>");
MODULE_DESCRIPTION("Voodoo3 I2C/SMBus driver");

int init_module(void)
{
  return voodoo3_init();
}

int cleanup_module(void)
{
  return voodoo3_cleanup();
}

#endif /* MODULE */

