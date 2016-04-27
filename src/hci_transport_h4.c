/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

/*
 *  hci_h4_transport.c
 *
 *  HCI Transport API implementation for basic H4 protocol over POSIX
 *
 *  Created by Matthias Ringwald on 4/29/09.
 */

#include "btstack_config.h"

#include "btstack_debug.h"
#include "hci.h"
#include "hci_transport.h"
#include "btstack_uart_block.h"

#ifdef HAVE_EHCILL
#error "HCI Transport H4 POSIX does not support eHCILL yet. Please remove HAVE_EHCILL from your btstack-config.h"
#endif 

// assert pre-buffer for packet type is available
#if !defined(HCI_OUTGOING_PRE_BUFFER_SIZE) || (HCI_OUTGOING_PRE_BUFFER_SIZE == 0)
#error HCI_OUTGOING_PRE_BUFFER_SIZE not defined. Please update hci.h
#endif

static void dummy_handler(uint8_t packet_type, uint8_t *packet, uint16_t size); 

typedef enum {
    H4_W4_PACKET_TYPE,
    H4_W4_EVENT_HEADER,
    H4_W4_ACL_HEADER,
    H4_W4_SCO_HEADER,
    H4_W4_PAYLOAD,
} H4_STATE;

// UART Driver + Config
static const btstack_uart_block_t * btstack_uart;
static btstack_uart_config_t uart_config;

// write mutex
static int uart_write_active;

static  void (*packet_handler)(uint8_t packet_type, uint8_t *packet, uint16_t size) = dummy_handler;

// packet reader state machine
static  H4_STATE h4_state;
static int bytes_to_read;
static int read_pos;

// incoming packet buffer
static uint8_t hci_packet_with_pre_buffer[HCI_INCOMING_PRE_BUFFER_SIZE + 1 + HCI_PACKET_BUFFER_SIZE]; // packet type + max(acl header + acl payload, event header + event data)
static uint8_t * hci_packet = &hci_packet_with_pre_buffer[HCI_INCOMING_PRE_BUFFER_SIZE];

static int hci_transport_h4_set_baudrate(uint32_t baudrate){
    log_info("hci_transport_h4_set_baudrate %u", baudrate);
    return btstack_uart->set_baudrate(baudrate);
}

static void hci_transport_h4_reset_statemachine(void){
    h4_state = H4_W4_PACKET_TYPE;
    read_pos = 0;
    bytes_to_read = 1;
}

static void hci_transport_h4_trigger_next_read(void){
    // trigger next read
    btstack_uart->receive_block(&hci_packet[read_pos], bytes_to_read);  
}

static void hci_transport_h4_block_sent(void){
    // free mutex
    uart_write_active = 0;

    // notify upper stack that it can send again
    uint8_t event[] = { HCI_EVENT_TRANSPORT_PACKET_SENT, 0};
    packet_handler(HCI_EVENT_PACKET, &event[0], sizeof(event));
}

static void hci_transport_h4_block_read(void){

    read_pos += bytes_to_read;

    switch (h4_state) {
        case H4_W4_PACKET_TYPE:
            switch (hci_packet[0]){
                case HCI_EVENT_PACKET:
                    bytes_to_read = HCI_EVENT_HEADER_SIZE;
                    h4_state = H4_W4_EVENT_HEADER;
                    break;
                case HCI_ACL_DATA_PACKET:
                    bytes_to_read = HCI_ACL_HEADER_SIZE;
                    h4_state = H4_W4_ACL_HEADER;
                    break;
                case HCI_SCO_DATA_PACKET:
                    bytes_to_read = HCI_SCO_HEADER_SIZE;
                    h4_state = H4_W4_SCO_HEADER;
                    break;
                default:
                    log_error("h4_process: invalid packet type 0x%02x", hci_packet[0]);
                    hci_transport_h4_reset_statemachine();
                    break;
            }
            break;
            
        case H4_W4_EVENT_HEADER:
            bytes_to_read = hci_packet[2];
            h4_state = H4_W4_PAYLOAD;
            break;
            
        case H4_W4_ACL_HEADER:
            bytes_to_read = little_endian_read_16( hci_packet, 3);
            // check ACL length
            if (HCI_ACL_HEADER_SIZE + bytes_to_read >  HCI_PACKET_BUFFER_SIZE){
                log_error("h4_process: invalid ACL payload len %u - only space for %u", bytes_to_read, HCI_PACKET_BUFFER_SIZE - HCI_ACL_HEADER_SIZE);
                hci_transport_h4_reset_statemachine();
                break;              
            }
            h4_state = H4_W4_PAYLOAD;
            break;
            
        case H4_W4_SCO_HEADER:
            bytes_to_read = hci_packet[3];
            h4_state = H4_W4_PAYLOAD;
            break;

        case H4_W4_PAYLOAD:
            packet_handler(hci_packet[0], &hci_packet[1], read_pos-1);
            hci_transport_h4_reset_statemachine();
            break;
        default:
            break;
    }
    hci_transport_h4_trigger_next_read();
}

static void hci_transport_h4_init(const void * transport_config){
    // check for hci_transport_config_uart_t
    if (!transport_config) {
        log_error("hci_transport_h4: no config!");
        return;
    }
    if (((hci_transport_config_t*)transport_config)->type != HCI_TRANSPORT_CONFIG_UART) {
        log_error("hci_transport_h4: config not of type != HCI_TRANSPORT_CONFIG_UART!");
        return;
    }

    // extract UART config from transport config
    hci_transport_config_uart_t * hci_transport_config_uart = (hci_transport_config_uart_t*) transport_config;
    uart_config.baudrate    = hci_transport_config_uart->baudrate_init;
    uart_config.flowcontrol = hci_transport_config_uart->flowcontrol;
    uart_config.device_name = hci_transport_config_uart->device_name;

    // setup UART driver
    btstack_uart->init(&uart_config);
    btstack_uart->set_block_received(&hci_transport_h4_block_read);
    btstack_uart->set_block_sent(&hci_transport_h4_block_sent);
}

static int hci_transport_h4_open(void){
    int res = btstack_uart->open();
    if (res){
        return res;
    }
    hci_transport_h4_reset_statemachine();
    hci_transport_h4_trigger_next_read();
    return 0;
}

static int hci_transport_h4_close(void){
    return btstack_uart->close();
}

static void hci_transport_h4_register_packet_handler(void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size)){
    packet_handler = handler;
}

static int hci_transport_h4_can_send_now(uint8_t packet_type){
    return uart_write_active == 0;
}

static int hci_transport_h4_send_packet(uint8_t packet_type, uint8_t * packet, int size){
    // store packet type before actual data and increase size
    size++;
    packet--;
    *packet = packet_type;

    // lock mutex
    uart_write_active = 1;

    //
    btstack_uart->send_block(packet, size);
    return 0;
}

static void dummy_handler(uint8_t packet_type, uint8_t *packet, uint16_t size){
}

static const hci_transport_t hci_transport_h4 = {
    /* const char * name; */                                        "H4",
    /* void   (*init) (const void *transport_config); */            &hci_transport_h4_init,
    /* int    (*open)(void); */                                     &hci_transport_h4_open,
    /* int    (*close)(void); */                                    &hci_transport_h4_close,
    /* void   (*register_packet_handler)(void (*handler)(...); */   &hci_transport_h4_register_packet_handler,
    /* int    (*can_send_packet_now)(uint8_t packet_type); */       &hci_transport_h4_can_send_now,
    /* int    (*send_packet)(...); */                               &hci_transport_h4_send_packet,
    /* int    (*set_baudrate)(uint32_t baudrate); */                &hci_transport_h4_set_baudrate,
    /* void   (*reset_link)(void); */                               NULL,
};

// configure and return h4 singleton
const hci_transport_t * hci_transport_h4_instance(const btstack_uart_block_t * uart_driver) {
    btstack_uart = uart_driver;
    return &hci_transport_h4;
}