/*
 * Copyright (C) 2015 BlueKitchen GmbH
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
 *  hci_h4_transport_wiced.c
 *
 *  HCI Transport API implementation for basic H4 protocol for use with btstack_run_loop_wiced.c
 */

#define __BTSTACK_FILE__ "hci_transport_h4_wiced.c"

#include "btstack_config.h"
#include "btstack_run_loop_wiced.h"

#include "btstack_debug.h"
#include "hci.h"
#include "hci_transport.h"
#include "platform_bluetooth.h"

#include "wiced.h"

#include <stdio.h>
#include <string.h>

// priority higher than WIFI to make sure RTS is set
#define WICED_BT_UART_THREAD_PRIORITY        (WICED_NETWORK_WORKER_PRIORITY - 2)
#define WICED_BT_UART_THREAD_STACK_SIZE      300

// assert pre-buffer for packet type is available
#if !defined(HCI_OUTGOING_PRE_BUFFER_SIZE) || (HCI_OUTGOING_PRE_BUFFER_SIZE == 0)
#error HCI_OUTGOING_PRE_BUFFER_SIZE not defined. Please update hci.h
#endif

/* Default of 512 bytes should be fine. Only needed if WICED_BT_UART_MANUAL_CTS_RTS */
#ifndef RX_RING_BUFFER_SIZE
#define RX_RING_BUFFER_SIZE 512
#endif

static wiced_result_t btstack_uart_block_wiced_rx_worker_receive_block(void * arg);

static wiced_worker_thread_t tx_worker_thread;
static const uint8_t *       tx_worker_data_buffer;
static uint16_t              tx_worker_data_size;

static wiced_worker_thread_t rx_worker_thread;
static uint8_t *             rx_worker_read_buffer;
static uint16_t              rx_worker_read_size;

#ifdef WICED_BT_UART_MANUAL_CTS_RTS
static volatile wiced_ring_buffer_t rx_ring_buffer;
static volatile uint8_t             rx_data[RX_RING_BUFFER_SIZE];
#endif

// uart config
static const btstack_uart_config_t * uart_config;

// callbacks
static void (*block_sent)(void);
static void (*block_received)(void);

// executed on main run loop
static wiced_result_t btstack_uart_block_wiced_main_notify_block_send(void *arg){
    if (block_sent){
        block_sent();
    }
    return WICED_SUCCESS;
}

// executed on main run loop
static wiced_result_t btstack_uart_block_wiced_main_notify_block_read(void *arg){
    if (block_received){
        block_received();
    }
    return WICED_SUCCESS;
}

// executed on tx worker thread
static wiced_result_t btstack_uart_block_wiced_tx_worker_send_block(void * arg){
#ifdef WICED_BT_UART_MANUAL_CTS_RTS
    while (platform_gpio_input_get(wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS]) == WICED_TRUE){
        wiced_rtos_delay_milliseconds(100);
    }
#endif
    // blocking send
    platform_uart_transmit_bytes(wiced_bt_uart_driver, tx_worker_data_buffer, tx_worker_data_size);
    // let transport know
    btstack_run_loop_wiced_execute_code_on_main_thread(&btstack_uart_block_wiced_main_notify_block_send, NULL);
    return WICED_SUCCESS;
}

// executed on rx worker thread
static wiced_result_t btstack_uart_block_wiced_rx_worker_receive_block(void * arg){

#ifdef WICED_BT_UART_MANUAL_CTS_RTS
    platform_gpio_output_low(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS]);
#endif

#ifdef WICED_UART_READ_DOES_NOT_RETURN_BYTES_READ
    // older API passes in number of bytes to read (checked in 3.3.1 and 3.4.0)
    platform_uart_receive_bytes(wiced_bt_uart_driver, rx_worker_read_buffer, rx_worker_read_size, WICED_NEVER_TIMEOUT);
#else
    // newer API uses pointer to return number of read bytes
    uint32_t bytes = rx_worker_read_size;
    platform_uart_receive_bytes(wiced_bt_uart_driver, rx_worker_read_buffer, &bytes, WICED_NEVER_TIMEOUT);
    // assumption: bytes = bytes_to_read as timeout is never    
#endif

#ifdef WICED_BT_UART_MANUAL_CTS_RTS
        platform_gpio_output_high(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS]);
#endif

    // let transport know
    btstack_run_loop_wiced_execute_code_on_main_thread(&btstack_uart_block_wiced_main_notify_block_read, NULL);
    return WICED_SUCCESS;
}

static int btstack_uart_block_wiced_init(const btstack_uart_config_t * config){
    uart_config = config;
    return 0;
}

static int btstack_uart_block_wiced_open(void){

    // UART config
    wiced_uart_config_t uart_config =
    {
        .baud_rate    = 115200,
        .data_width   = DATA_WIDTH_8BIT,
        .parity       = NO_PARITY,
        .stop_bits    = STOP_BITS_1,
        .flow_control = FLOW_CONTROL_CTS_RTS,
    };
    wiced_ring_buffer_t * ring_buffer = NULL;

    // configure HOST and DEVICE WAKE PINs
    platform_gpio_init(wiced_bt_control_pins[WICED_BT_PIN_HOST_WAKE], INPUT_HIGH_IMPEDANCE);
    platform_gpio_init(wiced_bt_control_pins[WICED_BT_PIN_DEVICE_WAKE], OUTPUT_PUSH_PULL);
    platform_gpio_output_low(wiced_bt_control_pins[WICED_BT_PIN_DEVICE_WAKE]);

    /* Configure Reg Enable pin to output. Set to HIGH */
    if (wiced_bt_control_pins[ WICED_BT_PIN_POWER ]){
        platform_gpio_init( wiced_bt_control_pins[ WICED_BT_PIN_POWER ], OUTPUT_OPEN_DRAIN_PULL_UP );
        platform_gpio_output_high( wiced_bt_control_pins[ WICED_BT_PIN_POWER ] );
    }

    wiced_rtos_delay_milliseconds( 100 );

    // -- init UART
#ifdef WICED_BT_UART_MANUAL_CTS_RTS
    // configure RTS pin as output and set to high
    platform_gpio_init(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS], OUTPUT_PUSH_PULL);
    platform_gpio_output_high(wiced_bt_uart_pins[WICED_BT_PIN_UART_RTS]);

    // configure CTS to input, pull-up
    platform_gpio_init(wiced_bt_uart_pins[WICED_BT_PIN_UART_CTS], INPUT_PULL_UP);

    // use ring buffer to allow to receive RX_RING_BUFFER_SIZE/2 addition bytes before raising RTS
    // casts avoid warnings because of volatile qualifier
    ring_buffer_init((wiced_ring_buffer_t *) &rx_ring_buffer, (uint8_t*) rx_data, sizeof( rx_data ) );
    ring_buffer = (wiced_ring_buffer_t *) &rx_ring_buffer;

    // don't try
    uart_config.flow_control = FLOW_CONTROL_DISABLED;
#endif
    platform_uart_init( wiced_bt_uart_driver, wiced_bt_uart_peripheral, &uart_config, ring_buffer );


    // Reset Bluetooth via RESET line. Fallback to toggling POWER otherwise
    if ( wiced_bt_control_pins[ WICED_BT_PIN_RESET ]){
        platform_gpio_init( wiced_bt_control_pins[ WICED_BT_PIN_RESET ], OUTPUT_PUSH_PULL );
        platform_gpio_output_high( wiced_bt_control_pins[ WICED_BT_PIN_RESET ] );

        platform_gpio_output_low( wiced_bt_control_pins[ WICED_BT_PIN_RESET ] );
        wiced_rtos_delay_milliseconds( 100 );
        platform_gpio_output_high( wiced_bt_control_pins[ WICED_BT_PIN_RESET ] );
    }
    else if ( wiced_bt_control_pins[ WICED_BT_PIN_POWER ]){
        platform_gpio_output_low( wiced_bt_control_pins[ WICED_BT_PIN_POWER ] );
        wiced_rtos_delay_milliseconds( 100 );
        platform_gpio_output_high( wiced_bt_control_pins[ WICED_BT_PIN_POWER ] );
    }

    // wait for Bluetooth to start up
    wiced_rtos_delay_milliseconds( 500 );

    // create worker threads for rx/tx. only single request is posted to their queues
    wiced_rtos_create_worker_thread(&tx_worker_thread, WICED_BT_UART_THREAD_PRIORITY, WICED_BT_UART_THREAD_STACK_SIZE, 1);
    wiced_rtos_create_worker_thread(&rx_worker_thread, WICED_BT_UART_THREAD_PRIORITY, WICED_BT_UART_THREAD_STACK_SIZE, 1);

    // tx is ready
    tx_worker_data_size = 0;
    return 0;
}

static int btstack_uart_block_wiced_close(void){
    // not implemented
    return 0;
}

static void btstack_uart_block_wiced_set_block_received( void (*block_handler)(void)){
    block_received = block_handler;
}

static void btstack_uart_block_wiced_set_block_sent( void (*block_handler)(void)){
    block_sent = block_handler;
}

static int btstack_uart_block_wiced_set_baudrate(uint32_t baudrate){

#if defined(_STM32F205RGT6_) || defined(STM32F40_41xxx)

    // directly use STM peripheral functions to change baud rate dynamically
    
    // set TX to high
    log_info("h4_set_baudrate %u", (int) baudrate);
    const platform_gpio_t* gpio = wiced_bt_uart_pins[WICED_BT_PIN_UART_TX];
    platform_gpio_output_high(gpio);

    // reconfigure TX pin as GPIO
    GPIO_InitTypeDef gpio_init_structure;
    gpio_init_structure.GPIO_Speed = GPIO_Speed_50MHz;
    gpio_init_structure.GPIO_Mode  = GPIO_Mode_OUT;
    gpio_init_structure.GPIO_OType = GPIO_OType_PP;
    gpio_init_structure.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gpio_init_structure.GPIO_Pin   = (uint32_t) ( 1 << gpio->pin_number );
    GPIO_Init( gpio->port, &gpio_init_structure );

    // disable USART
    USART_Cmd( wiced_bt_uart_peripheral->port, DISABLE );

    // setup init structure
    USART_InitTypeDef uart_init_structure;
    uart_init_structure.USART_Mode       = USART_Mode_Rx | USART_Mode_Tx;
    uart_init_structure.USART_BaudRate   = baudrate;
    uart_init_structure.USART_WordLength = USART_WordLength_8b;
    uart_init_structure.USART_StopBits   = USART_StopBits_1;
    uart_init_structure.USART_Parity     = USART_Parity_No;
    uart_init_structure.USART_HardwareFlowControl = USART_HardwareFlowControl_RTS_CTS;
#ifdef WICED_BT_UART_MANUAL_CTS_RTS
    uart_init_structure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
#endif
    USART_Init(wiced_bt_uart_peripheral->port, &uart_init_structure);

    // enable USART again
    USART_Cmd( wiced_bt_uart_peripheral->port, ENABLE );

    // set TX pin as USART again
    gpio_init_structure.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_Init( gpio->port, &gpio_init_structure );

#else
    log_error("btstack_uart_block_wiced_set_baudrate not implemented for this WICED Platform");
#endif
    return 0;
}

static int btstack_uart_block_wiced_set_parity(int parity){
    log_error("btstack_uart_block_wiced_set_parity not implemented");
    return 0;
}

static void btstack_uart_block_wiced_send_block(const uint8_t *buffer, uint16_t length){
    // store in request
    tx_worker_data_buffer = buffer;
    tx_worker_data_size = length;
    wiced_rtos_send_asynchronous_event(&tx_worker_thread, &btstack_uart_block_wiced_tx_worker_send_block, NULL);    
}

static void btstack_uart_block_wiced_receive_block(uint8_t *buffer, uint16_t len){
    rx_worker_read_buffer = buffer;
    rx_worker_read_size   = len;
    wiced_rtos_send_asynchronous_event(&rx_worker_thread, &btstack_uart_block_wiced_rx_worker_receive_block, NULL);    
}


// static void btstack_uart_block_wiced_set_sleep(uint8_t sleep){
// }
// static void btstack_uart_block_wiced_set_csr_irq_handler( void (*csr_irq_handler)(void)){
// }

static const btstack_uart_block_t btstack_uart_block_wiced = {
    /* int  (*init)(hci_transport_config_uart_t * config); */         &btstack_uart_block_wiced_init,
    /* int  (*open)(void); */                                         &btstack_uart_block_wiced_open,
    /* int  (*close)(void); */                                        &btstack_uart_block_wiced_close,
    /* void (*set_block_received)(void (*handler)(void)); */          &btstack_uart_block_wiced_set_block_received,
    /* void (*set_block_sent)(void (*handler)(void)); */              &btstack_uart_block_wiced_set_block_sent,
    /* int  (*set_baudrate)(uint32_t baudrate); */                    &btstack_uart_block_wiced_set_baudrate,
    /* int  (*set_parity)(int parity); */                             &btstack_uart_block_wiced_set_parity,
    /* void (*receive_block)(uint8_t *buffer, uint16_t len); */       &btstack_uart_block_wiced_receive_block,
    /* void (*send_block)(const uint8_t *buffer, uint16_t length); */ &btstack_uart_block_wiced_send_block,
    /* int (*get_supported_sleep_modes); */                           NULL,
    /* void (*set_sleep)(btstack_uart_sleep_mode_t sleep_mode); */    NULL
};

const btstack_uart_block_t * btstack_uart_block_wiced_instance(void){
    return &btstack_uart_block_wiced;
}