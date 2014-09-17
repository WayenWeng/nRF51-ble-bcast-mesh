#include "radio_control.h"
#include "trickle.h"
#include "trickle_common.h"


#include "nrf_sdm.h"
#include "app_error.h"
#include "nrf_assert.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "nrf_soc.h"
#include "boards.h"
#include "simple_uart.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>


/*****************************************************************************
* Local type definitions
*****************************************************************************/

typedef enum
{
    TRICKLE_RADIO_MODE_SEARCHING,
    TRICKLE_RADIO_MODE_PERIODIC
} trickle_radio_mode_t;


/*****************************************************************************
* Static globals
*****************************************************************************/

/**
* Timeslot request structures
*/
static nrf_radio_request_t radio_request_normal = 
                {
                    .request_type = NRF_RADIO_REQ_TYPE_NORMAL,
                    .params.normal = 
                    {
                        .hfclk = NRF_RADIO_HFCLK_CFG_DEFAULT,
                        .priority = NRF_RADIO_PRIORITY_NORMAL,
                        .distance_us = TRICKLE_INTERVAL_US,
                        .length_us = TRICKLE_TIMESLOT_LENGTH_US
                    }
                };
                
static nrf_radio_request_t radio_request_earliest = 
                {
                    .request_type = NRF_RADIO_REQ_TYPE_EARLIEST,
                    .params.earliest = 
                    {
                        .hfclk = NRF_RADIO_HFCLK_CFG_DEFAULT,
                        .priority = NRF_RADIO_PRIORITY_NORMAL,
                        .length_us = TRICKLE_TIMESLOT_LENGTH_US,
                        .timeout_us = 1000000 /* 1s */
                    }
                };
                        
                
/**
* Timeslot callback return parameters
*/
static const nrf_radio_signal_callback_return_param_t radio_signal_cb_ret_param_none =
                {
                    .callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE
                };
                
static nrf_radio_signal_callback_return_param_t radio_signal_cb_ret_param_request =
                {
                    .callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_REQUEST_AND_END,
                    .params.request.p_next = (nrf_radio_request_t*) &radio_request_normal
                };

                
          
static trickle_radio_mode_t g_trickle_radio_mode;
/*****************************************************************************
* Static Functions
*****************************************************************************/

/**
* Causes the unit to enter scan mode, where it will try to sync up with an 
* existing network
*
* Puts the radio in RX mode and stays there, either until a packet is received
* or until the search timeout is triggered. Enables a PPI from address match to 
* timer capture, in order to capture the RX time as precisely as possible.
* When the radio receives a valid packet, it enters periodic mode, which is the 
* main operation mode. This is done by ordering a timeslot at 10ms (minus setup and
* propagation time) from the beginning of the packet reception. 
*/
static void enter_search_mode(void)
{
    radio_request_earliest.params.earliest.length_us = 100000; /* 100ms */
    sd_radio_request((nrf_radio_request_t*) &radio_request_earliest);
    g_trickle_radio_mode = TRICKLE_RADIO_MODE_SEARCHING;
}

/**
* Called when the searching timeslot has been granted. Sets up radio, timer and PPI
*/
static void search_mode_timeslot_started(void)
{
    /* Setup timeout timer */
    NRF_TIMER0->POWER = 1;
    NRF_TIMER0->TASKS_CLEAR = 1;
    NRF_TIMER0->PRESCALER = 4; /* 1MHz */
    NRF_TIMER0->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER0->CC[0] = 50000;
    NRF_TIMER0->CC[1] = 0;
    NRF_TIMER0->EVENTS_COMPARE[0] = 0;
    NRF_TIMER0->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
    NRF_TIMER0->TASKS_START = 1;
    NVIC_EnableIRQ(TIMER0_IRQn);
    
    /* Setup PPI */
    NRF_PPI->CH[TRICKLE_SEARCHING_TIMEOUT_PPI_CH].EEP = (uint32_t) &(NRF_RADIO->EVENTS_ADDRESS);
	NRF_PPI->CH[TRICKLE_SEARCHING_TIMEOUT_PPI_CH].TEP = (uint32_t) &(NRF_TIMER0->TASKS_CAPTURE[1]);
	NRF_PPI->CHENSET 			 |= (1 << TRICKLE_SEARCHING_TIMEOUT_PPI_CH);
}

/*****************************************************************************
* System callback functions
*****************************************************************************/

/**
* Callback for data reception. Called from radio_event_handler in radio_control.c
* upon packet reception. Called in LowerStack interrupt priority (from radio_signal_callback)
*/
void radio_rx_callback(uint8_t* rx_data)
{
    /*TODO: Handle incoming messages */
}

void sd_assert_handler(uint32_t pc, uint16_t line_num, const uint8_t* p_file_name)
{
    SET_PIN(PIN_ABORTED);
    while (true)
    {
        nrf_delay_ms(500);
        SET_PIN(LED_0);
        CLEAR_PIN(LED_1);
        nrf_delay_ms(500);
        SET_PIN(LED_1);
        CLEAR_PIN(LED_0);
    }
}

void app_error_handler(uint32_t error_code, uint32_t line_num, const uint8_t * p_file_name)
{
    SET_PIN(PIN_ABORTED);
    while (true)
    {
        nrf_delay_ms(500);
        SET_PIN(LED_0);
        CLEAR_PIN(LED_1);
        nrf_delay_ms(500);
        SET_PIN(LED_1);
        CLEAR_PIN(LED_0);
    }
}

/**
* Timeslot related events callback
* Called whenever the softdevice tries to change the original course of actions 
* related to the timeslots
*/
void SD_EVT_IRQHandler(void)
{
    uint32_t evt;
    
    while (sd_evt_get(&evt) == NRF_SUCCESS)
    {
        switch (evt)
        {
            case NRF_EVT_RADIO_SESSION_IDLE:
                
                APP_ERROR_CHECK(NRF_ERROR_INVALID_DATA);
                break;
            
            case NRF_EVT_RADIO_SESSION_CLOSED:
                APP_ERROR_CHECK(NRF_ERROR_INVALID_DATA);
                
                break;
            
            case NRF_EVT_RADIO_BLOCKED:
                APP_ERROR_CHECK(NRF_ERROR_INVALID_DATA);
                
                break;
            
            case NRF_EVT_RADIO_SIGNAL_CALLBACK_INVALID_RETURN:
                APP_ERROR_CHECK(NRF_ERROR_INVALID_DATA);
                break;
            
            case NRF_EVT_RADIO_CANCELED:
                
                APP_ERROR_CHECK(NRF_ERROR_INVALID_DATA);
                break;
            
            default:
                APP_ERROR_CHECK(NRF_ERROR_INVALID_STATE);
        }
    }
}


/**
* Radio signal callback handler taking care of all signals in searching mode
*/
static nrf_radio_signal_callback_return_param_t* radio_signal_callback_searching(uint8_t sig)
{
    /* If the trickle step is not finished, the default action is to continue the timeslot */
    nrf_radio_signal_callback_return_param_t* ret_param = (nrf_radio_signal_callback_return_param_t*) &radio_signal_cb_ret_param_none;
    
    switch (sig)
    {
        case NRF_RADIO_CALLBACK_SIGNAL_TYPE_START:
            /* timeslot start, init radio module */
            radio_init(&radio_rx_callback);
            search_mode_timeslot_started();
            SET_PIN(PIN_SEARCHING);
            /* send radio into continuous rx mode */
            //radio_rx(0);
        
            break;
        case NRF_RADIO_CALLBACK_SIGNAL_TYPE_RADIO:
            /* send to radio control module */
            radio_event_handler();
            break;
        case NRF_RADIO_CALLBACK_SIGNAL_TYPE_TIMER0:
            /* give up timer, start periodic */
            TICK_PIN(PIN_CONSISTENT);
            CLEAR_PIN(PIN_SEARCHING);
            if (NRF_TIMER0->EVENTS_COMPARE[0])
            {
                radio_disable();
                radio_signal_cb_ret_param_request.params.request.p_next = (nrf_radio_request_t*) &radio_request_earliest;
                ret_param = &radio_signal_cb_ret_param_request;
                NRF_TIMER0->TASKS_STOP = 1;
                g_trickle_radio_mode = TRICKLE_RADIO_MODE_PERIODIC;
            }
                
            break;
        default:
            APP_ERROR_CHECK(NRF_ERROR_INVALID_STATE);
    }
        
    return ret_param;
}


/**
* Radio signal callback handler taking care of all signals in periodic mode
*/
static nrf_radio_signal_callback_return_param_t* radio_signal_callback_periodic(uint8_t sig)
{
    /* If the trickle step is not finished, the default action is to continue the timeslot */
    nrf_radio_signal_callback_return_param_t* ret_param = (nrf_radio_signal_callback_return_param_t*) &radio_signal_cb_ret_param_none;
    
    switch (sig)
    {
        case NRF_RADIO_CALLBACK_SIGNAL_TYPE_START:
            /* timeslot start, init radio module */
            radio_init(&radio_rx_callback);
        
            TICK_PIN(PIN_SYNC_TIME);
#if 0
            trickle_tx_cb tx_func = trickle_step();
            
            if (tx_func == NULL)
            {
                /* No tx this step, go into rx mode */
                radio_rx(1);
                
                
            }
            tx_func();
#endif            
            radio_request_normal.params.normal.distance_us = TRICKLE_INTERVAL_US;
            radio_signal_cb_ret_param_request.params.request.p_next = &radio_request_normal;
            ret_param = &radio_signal_cb_ret_param_request;
        
            break;
        case NRF_RADIO_CALLBACK_SIGNAL_TYPE_RADIO:
            /* send to radio control module */
            radio_event_handler();
            break;
        case NRF_RADIO_CALLBACK_SIGNAL_TYPE_TIMER0:
            /* TODO */
            break;
        default:
            APP_ERROR_CHECK(NRF_ERROR_INVALID_STATE);
    }
        
    return ret_param;
}


/**
* Callback function for radio signals in time slot. Delegates signal based on state
*/
static nrf_radio_signal_callback_return_param_t* radio_signal_callback(uint8_t sig)
{
    nrf_radio_signal_callback_return_param_t* ret_param = (nrf_radio_signal_callback_return_param_t*) &radio_signal_cb_ret_param_none;
    TICK_PIN(PIN_RADIO_SIGNAL);
    switch (g_trickle_radio_mode)
    {
        case TRICKLE_RADIO_MODE_SEARCHING:
            ret_param = radio_signal_callback_searching(sig);
            break;
        case TRICKLE_RADIO_MODE_PERIODIC:
            ret_param = radio_signal_callback_periodic(sig);
            break;
    }
    
    return ret_param;
}

/*****************************************************************************
* Interface Functions
*****************************************************************************/

void timeslot_handler_init(void)
{
    uint32_t error;
    
    error = sd_softdevice_enable((uint32_t)NRF_CLOCK_LFCLKSRC_XTAL_75_PPM, sd_assert_handler);
    APP_ERROR_CHECK(error);
    
    error = sd_nvic_EnableIRQ(SD_EVT_IRQn);
    APP_ERROR_CHECK(error);
    
    sd_radio_session_open(&radio_signal_callback);
    
    enter_search_mode();
    
}

void timeslot_handler_start_periodic(uint32_t time_period_us)
{
    if (time_period_us == 0)
    {
        APP_ERROR_CHECK(sd_radio_request((nrf_radio_request_t*) &radio_request_earliest));
    }
    else
    {
        radio_request_normal.params.normal.distance_us = time_period_us;
        APP_ERROR_CHECK(sd_radio_request(&radio_request_normal));
    }
    
    g_trickle_radio_mode = TRICKLE_RADIO_MODE_PERIODIC;
}

