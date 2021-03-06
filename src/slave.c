/*
 *  Universal rf24boot bootloader : Bootloader core
 *  Copyright (C) 2014  Andrew 'Necromant' Andrianov <andrew@ncrmnt.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <arch/antares.h>
#include <arch/delay.h>
#include <rf24boot.h>

#define DEBUG_LEVEL CONFIG_DEBUGGING_VERBOSITY
#define COMPONENT "rf24boot"

#include <lib/printk.h>
#include <lib/RF24.h>
#include <lib/panic.h>
#include <string.h>


#define CONFIG_BOOT_MAX_PARTS 3
static int partcount;
struct rf24boot_partition *parts[CONFIG_BOOT_MAX_PARTS];

void rf24boot_add_part(struct rf24boot_partition *part)
{
	dbg("Registering partition %d: %s\n", partcount, part->info.name);
	BUG_ON(partcount >= CONFIG_BOOT_MAX_PARTS); /* Increase max parts */
	parts[partcount++] = part;
}

void rf24boot_boot_by_name(char* name)
{
	uint8_t i;
	for (i=0; i<partcount; i++)
	{
		if (strcmp(name, parts[i]->info.name)==0)
			rf24boot_boot_partition(parts[i]);
	}

}

uint8_t g_rf24boot_got_hello = 0;

static uint8_t  local_addr[5] = { 
	CONFIG_RF_ADDR_0, 
	CONFIG_RF_ADDR_1, 
	CONFIG_RF_ADDR_2, 
	CONFIG_RF_ADDR_3, 
	CONFIG_RF_ADDR_4 
};


static struct rf24_config conf = {
	.channel = CONFIG_RF_CHANNEL,
	.pa = RF24_PA_MAX,
	.rate = RF24_2MBPS,
	.crclen = RF24_CRC_16,
	.dynamic_payloads = 1,
	.num_retries      = 15,
	.retry_timeout    = 15,
	.pipe_auto_ack    = 0xff,
	.payload_size     = 32
};

ANTARES_INIT_HIGH(slave_init) 
{

	rf24_init(g_radio); 
	rf24_config(g_radio, &conf);
	info("RF: init done\n");
	info("RF: module is %s P variant\n", rf24_is_p_variant(g_radio) ? "" : "NOT");
	dbg("Wireless in slave mode\n\n");
	rf24_print_details(g_radio);
	rf24_open_reading_pipe(g_radio, 0,  local_addr);
	rf24_start_listening(g_radio);
}


#define DEADTIME (CONFIG_DEADTIME_TIMEOUT * 1000) 
static uint8_t listening = 1;

static void listen(uint8_t state) { 
	if (listening && (!state)) { 
		rf24_stop_listening(g_radio);
		listening = 0;
	} else 	if (!listening && state) { 
		if (0==rf24_queue_sync(g_radio, 250))
			rf24boot_platform_reset();
		rf24_start_listening(g_radio); 
		listening=1;
	}
}


static void respond(uint8_t op, struct rf24boot_cmd *cmd, uint8_t len)
{
	listen(0);
	int ret;
#if CONFIG_HAVE_DEADTIME
	uint8_t retry = 0xff; 
	do {
		ret = rf24_queue_push(g_radio, cmd, len + 1);
		if (ret == 0)
			break;
		delay_ms((DEADTIME / 0xff)); /* If stuck for >5 sec - reboot */
	} while (--retry);
	
	if (!retry) { 
		rf24boot_platform_reset();
	}
	
	printk("resp op %d retry %d len %d\n", op, retry, len + 1);
#else 
	do {
		ret = rf24_queue_push(g_radio, cmd, len + 1);
	}
	while (ret != 0);
#endif	
	
}



static inline void handle_cmd(struct rf24boot_cmd *cmd) {
	uint8_t cmdcode = cmd->op & 0x0f;
	uint8_t i; 
	struct rf24boot_data *dat;
	if (cmdcode == RF_OP_HELLO)
	{
		struct rf24boot_hello_resp *resp;
		dbg("hello from 0x%02x:0x%02x:0x%02x:0x%02x:0x%02x !\n", 
		    cmd->data[0],
		    cmd->data[1],
		    cmd->data[2],
		    cmd->data[3],
		    cmd->data[4]
			);
		rf24_open_writing_pipe(g_radio, cmd->data);
		resp = (struct rf24boot_hello_resp *) cmd->data;		
		resp->numparts = partcount;
		resp->is_big_endian = 0; /* FixMe: ... */
		strncpy(resp->id, CONFIG_SLAVE_ID, 28);
		resp->id[28]=0x0;
		rf24_flush_rx(g_radio);
		respond(RF_OP_HELLO, cmd, 
			sizeof(struct rf24boot_hello_resp));
		g_rf24boot_got_hello=1;
		
		/* Shit out partition table */
		for (i=0; i< partcount; i++) {
			memcpy(cmd->data, (uint8_t *) &parts[i]->info, 
			       sizeof(struct rf24boot_partition_header));
			respond(RF_OP_PARTINFO, cmd, sizeof(struct rf24boot_partition_header));		
		}
		listen(1);
		
		return; 
	}
 

	dat = (struct rf24boot_data *) cmd->data;
	
	if (dat->part >= partcount)
		return; 

	if ((cmdcode == RF_OP_READ))
	{
		int ret;
		uint32_t toread = dat->addr;
		dat->addr = 0;
		do {
			ret = parts[dat->part]->read(parts[dat->part], dat->addr, dat->data);
			respond(RF_OP_READ, cmd, 
				ret + 5);
			dat->addr += (uint32_t) ret;	
		} while (dat->addr < toread);
		listen(1);
	} else if (cmdcode == RF_OP_WRITE)
	{
		dbg("write addr: %lu\n", dat->addr);
		parts[dat->part]->write(parts[dat->part], dat->addr, dat->data);
	} else if ((cmdcode == RF_OP_BOOT))
	{
		rf24boot_boot_partition(parts[dat->part]);
	}
}


ANTARES_APP(slave)
{
	struct rf24boot_cmd cmd; /* The hw fifo is 3 levels deep */
	uint8_t pipe; 
	if (rf24_available(g_radio, &pipe)) {
			uint8_t len = rf24_get_dynamic_payload_size(g_radio);
			rf24_read(g_radio, &cmd, len);
			printk("> got packet, len %d\n", len);
			dbg("\ngot cmd: %x \n", cmd.op);
			handle_cmd(&cmd);
	}
}

#ifdef CONFIG_TOOLCHAIN_SDCC
int main() {
	do_antares_startup();
}
#endif
