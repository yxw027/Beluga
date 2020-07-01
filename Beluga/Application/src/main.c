/** @file   main.c
 *
 *  @brief  Beluga main program file
 *
 *  @date   2020/06
 *
 *  @author WiseLab-CMU 
 */

/* Inlcudes for BLE library */
#include "ble_types.h"
#include "ble_gap.h"
#include "app_timer.h"

/* Inlcudes for UWB library */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "bsp.h"
#include "boards.h"
#include "nrf_drv_clock.h"
#include "nrf_drv_spi.h"
#include "nrf_uart.h"
#include "app_util_platform.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "nrf.h"
#include "app_error.h"
#include "app_uart.h"
#include "port_platform.h"
#include "deca_types.h"
#include "deca_param_types.h"
#include "deca_regs.h"
#include "deca_device_api.h"

#include "nrf_drv_gpiote.h"
#include "ss_init_main.h"
#include "port_platform.h"
#include "semphr.h"
#include "nrf_fstorage_sd.h"
#include "nrf_soc.h"

/* Customer header files */
#include "flash.h"
#include "uart.h"
#include "ble_app.h"

#if defined (UART_PRESENT)
#include "nrf_uart.h"
#endif
#if defined (UARTE_PRESENT)
#include "nrf_uarte.h"
#endif

/* Firmware version */
#define FIRMWARE_VERSION "1.0"

/* Maximum transmission power register value */
#define TX_POWER_MAX 0x1F1F1F1F

/* Preamble timeout, in multiple of PAC size. See NOTE 3 below. */
#define PRE_TIMEOUT 1000

/* Delay between frames, in UWB microseconds. See NOTE 1 below. */
#define POLL_TX_TO_RESP_RX_DLY_UUS 100 


static int mode;

extern ble_uuid_t m_adv_uuids[2];
extern node seen_list[MAX_ANCHOR_COUNT];
extern int ble_started;
static int uwb_started;
extern uint32_t time_keeper;
extern int node_added;

int debug_print;
int streaming_mode;

SemaphoreHandle_t rxSemaphore, txSemaphore, sus_resp, sus_init, print_list_sem;
QueueHandle_t uart_queue;

static int initiator_freq = 100;
static int time_out = 5000;


/*----------------- DW1000 Configuration ------------------*/

/* DW1000 config function */
dwt_config_t config = {
    5,                /* Channel number. */
    DWT_PRF_64M,      /* Pulse repetition frequency. */
    DWT_PLEN_128,     /* Preamble length. Used in TX only. */
    DWT_PAC8,         /* Preamble acquisition chunk size. Used in RX only. */
    10,               /* TX preamble code. Used in TX only. */
    10,               /* RX preamble code. Used in RX only. */
    0,                /* 0 to use standard SFD, 1 to use non-standard SFD. */
    DWT_BR_6M8,       /* Data rate. */
    DWT_PHRMODE_STD,  /* PHY header mode. */
    (129 + 8 - 8)     /* SFD timeout (preamble length + 1 + SFD length - PAC size). Used in RX only. */
};


/* DW1000 TX config struct */
dwt_txconfig_t config_tx = {
  TC_PGDELAY_CH5,
  TX_POWER_MAN_DEFAULT
};



//---------------------- Timer Functions -------------------------//

APP_TIMER_DEF(m_timestamp_keeper);    /**< Handler for repeated timer used to blink LED 1. */


/**@brief Function for initializing the timer.
 */
static void timer_init(void)
{

    //APP_TIMER_INIT(APP_TIMER_PRESCALER, APP_TIMER_OP_QUEUE_SIZE, false);
    ret_code_t err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);
}

/**@brief Timeout handler for the repeated timer.
 */
static void timestamp_handler(void * p_context)
{ 
    time_keeper += 1; 
}


//---------------------- UWB related Functions -------------------------//

void init_reconfig() {

  dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
  dwt_setrxtimeout(2000); // Maximum value timeout with DW1000 is 65ms  

}

void resp_reconfig() {

  dwt_setrxaftertxdelay(0);
  dwt_setrxtimeout(0);

}

uint16_t get_rand_num(uint32_t freq) {

      uint32_t lower = freq;
      uint32_t upper = freq + 50;
      uint8_t num_rand_bytes_available;
      int err_code = sd_rand_application_bytes_available_get(&num_rand_bytes_available);
      APP_ERROR_CHECK(err_code);
      uint8_t rand_number;
      if (num_rand_bytes_available > 0)
      {
          err_code = sd_rand_application_vector_get(&rand_number, 1);
          APP_ERROR_CHECK(err_code);
      }


      float rand = rand_number * 1.0;

      float r =  (rand - 0) * (upper - lower) / (255 - 0) + lower;
      uint16_t ret = (uint16_t) r;

      return ret;
}


//------------------------ Task Functions -------------------------------//

TaskHandle_t ss_responder_task_handle; 
TaskHandle_t ranging_task_handle;
TaskHandle_t ss_initiator_task_handle; 
TaskHandle_t uart_task_handle;
TaskHandle_t list_task_handle;
TaskHandle_t monitor_task_handle;
TaskHandle_t ble_task_handle;

void ble_task_fuction() {

  while(1) {
    if (debug_print == 1) printf("BLE task in \r\n");
    vTaskDelay(100);
    (void) sd_ble_gap_scan_stop();
    scan_start();
    if (debug_print == 1) printf("BLE task out \r\n");
  }

}

/**
 * @brief Output visible nodes information
 */
void list_task_function()
{

  while(1){
      
      vTaskDelay(50);
      if (debug_print == 1) printf("list task in \r\n");
      
      xSemaphoreTake(print_list_sem, portMAX_DELAY);
      
      message new_message = {0};

      /* Normal mode to print all neighbor nodes */
      if (streaming_mode == 0) {
        printf("# ID, RANGE, RSSI, TIMESTAMP\r\n");

        for(int j = 0; j < MAX_ANCHOR_COUNT; j++)
        {
          if(seen_list[j].UUID != 0) printf("%d, %f, %d, %d \r\n", seen_list[j].UUID, seen_list[j].range, seen_list[j].RSSI, seen_list[j].time_stamp); 
          //if(seen_list[j].UUID != 0) printf("%f \r\n", seen_list[j].range); 
        }
      }

      /* Streaming mode to print only new updated nodes */
      if (streaming_mode == 1) {
        int count_flag = 0;

        // Check whether alive nodes have update flag or not
        for(int i = 0; i < MAX_ANCHOR_COUNT; i++)
        {
          if(seen_list[i].UUID != 0 && seen_list[i].update_flag != 0) {
            count_flag++;
          }      
        }
        // If one of node has update flag, print it
        if (count_flag != 0) {
          printf("# ID, RANGE, RSSI, TIMESTAMP\r\n");

          for(int j = 0; j < MAX_ANCHOR_COUNT; j++)
          {
            if(seen_list[j].UUID != 0 && seen_list[j].update_flag == 1) printf("%d, %f, %d, %d \r\n", seen_list[j].UUID, seen_list[j].range, seen_list[j].RSSI, seen_list[j].time_stamp); 
            
            // Reset update flag of the node
            seen_list[j].update_flag = 0;
          }
        }
      }
      
      if (debug_print == 1) printf("list task out \r\n");
      xSemaphoreGive(print_list_sem);
   }
}


/**
 * @brief UART parser
 */
void uart_task_function(void * pvParameter){

  UNUSED_PARAMETER(pvParameter);

  message incoming_message = {0};

  while(1) {
    vTaskDelay(100);
    if (debug_print == 1) printf("uart task in \r\n");
    
    if(xQueueReceive(uart_queue, &incoming_message, 0) == pdPASS) {  

      //Handle valid AT command begining with AT+

      if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+", (size_t)3)) {
        
        //Handle specific AT commands
        if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+STARTUWB", (size_t)11)) {
            
            uwb_started = 1;
            // Give the suspension semaphore so UWB can continue
            xSemaphoreGive(sus_resp);
            xSemaphoreGive(sus_init);
            bsp_board_led_on(BSP_BOARD_LED_2);
            printf("OK \r\n");
           
        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+STOPUWB", (size_t)10)) {
            
            uwb_started = 0;
            // Take UWB suspension semaphore
            xSemaphoreTake(sus_resp, portMAX_DELAY);
            xSemaphoreTake(sus_init, portMAX_DELAY);
            bsp_board_led_off(BSP_BOARD_LED_2);
            printf("OK \r\n");
            
        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+STARTBLE", (size_t)11)) {
            
            ble_started = 1;
            // Give print list semaphore
            xSemaphoreGive(print_list_sem);
            adv_scan_start();
            bsp_board_led_on(BSP_BOARD_LED_1);
            printf("OK \r\n");
                    
        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+STOPBLE", (size_t)10)) {
            
            ble_started = 0;
            sd_ble_gap_adv_stop();
            sd_ble_gap_scan_stop();
            // Take print list semaphore to stop printing
            xSemaphoreTake(print_list_sem, portMAX_DELAY);
            bsp_board_led_off(BSP_BOARD_LED_1);
            printf("OK \r\n");
        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+ID", (size_t)5)) {
            char buf[100];
            strcpy(buf, incoming_message.data);
            char *uuid_char = strtok(buf, " ");
            uuid_char = strtok(NULL, " ");
            uint32_t rec_uuid = atoi(uuid_char);
            
            NODE_UUID = rec_uuid;
            m_adv_uuids[1].uuid = NODE_UUID;

            // Setup UUID into BLE 
            advertising_init();
            writeFlashID(rec_uuid, 1);
            printf("OK\r\n");
            
        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+BOOTMODE", (size_t)11)) {
            char buf[100];
            strcpy(buf, incoming_message.data);
            char *uuid_char = strtok(buf, " ");
            uuid_char = strtok(NULL, " ");
            uint32_t mode = atoi(uuid_char);
            
            if (mode < 0 || mode > 2) {
              printf("Invalid bootmode parameter \r\n");
            }
            else {
              if (mode == 1) {
                writeFlashID(1, 2);
              }
              else if (mode == 2) {
                 writeFlashID(2, 2);
              }
              else {
                writeFlashID(0, 2);
              }

              printf("Bootmode: %d OK \r\n", mode);
            }
        }

        else if(0 == strncmp((const char *)incoming_message.data, (const char *)"AT+RATE", (size_t)7)) {
            char buf[100];
            strcpy(buf, incoming_message.data);
            char *uuid_char = strtok(buf, " ");
            uuid_char = strtok(NULL, " ");
            uint32_t rate = atoi(uuid_char);
            
            if (rate < 0 || rate > 500) {
              printf("Invalid rate parameter \r\n"); 
            }
            else {
              writeFlashID(rate, 3);
              initiator_freq = rate;
              printf("Rate: %d OK \r\n", rate);
            }
        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+CHANNEL", (size_t)10)) {
            char buf[100];
            strcpy(buf, incoming_message.data);
            char *uuid_char = strtok(buf, " ");
            uuid_char = strtok(NULL, " ");
            uint32_t channel = atoi(uuid_char);

            if (channel < 1 || channel > 7 || channel == 6) {
              printf("Invalid Channel number \r\n");
            }
            else {
              writeFlashID(channel, 4);
              config.chan = channel;
              dwt_configure(&config);
            
              printf("OK \r\n");
            }

        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+RESET", (size_t)8)) {

          fds_record_desc_t   record_desc_1;
          fds_find_token_t    ftok_1;
          memset(&ftok_1, 0x00, sizeof(fds_find_token_t));
          if (fds_record_find(FILE_ID, RECORD_KEY_1, &record_desc_1, &ftok_1) == FDS_SUCCESS) //If there is a stored rate
            {};
          ret_code_t ret = fds_record_delete(&record_desc_1);
          if (ret != FDS_SUCCESS)
          {
             printf("FDS Delete error \r\n");
          }

          // Delete rate record
          fds_record_desc_t   record_desc_2;
          fds_find_token_t    ftok_2;
          memset(&ftok_2, 0x00, sizeof(fds_find_token_t));
          if (fds_record_find(FILE_ID, RECORD_KEY_3, &record_desc_2, &ftok_2) == FDS_SUCCESS) //If there is a stored rate
            {};
          ret_code_t ret2 = fds_record_delete(&record_desc_2);
          if (ret2 != FDS_SUCCESS)
          {
             printf("FDS Delete error \r\n");
          }

          // Delete channel record
          fds_record_desc_t   record_desc_3;
          fds_find_token_t    ftok_3;
          memset(&ftok_3, 0x00, sizeof(fds_find_token_t));
          if (fds_record_find(FILE_ID, RECORD_KEY_4, &record_desc_3, &ftok_3) == FDS_SUCCESS) //If there is a stored rate
            {};
          ret_code_t ret3 = fds_record_delete(&record_desc_3);
          if (ret3 != FDS_SUCCESS)
          {
             printf("FDS Delete error \r\n");
          }

          // Delete timeout record
          fds_record_desc_t   record_desc_4;
          fds_find_token_t    ftok_4;
          memset(&ftok_4, 0x00, sizeof(fds_find_token_t));
          if (fds_record_find(FILE_ID, RECORD_KEY_5, &record_desc_4, &ftok_4) == FDS_SUCCESS) //If there is a stored rate
            {};
          ret_code_t ret4 = fds_record_delete(&record_desc_4);
          if (ret4 != FDS_SUCCESS)
          {
             printf("FDS Delete error \r\n");
          }
          // Delete Tx power record
          fds_record_desc_t   record_desc_5;
          fds_find_token_t    ftok_5;
          memset(&ftok_5, 0x00, sizeof(fds_find_token_t));
          if (fds_record_find(FILE_ID, RECORD_KEY_6, &record_desc_5, &ftok_5) == FDS_SUCCESS) //If there is a stored rate
            {};
          ret_code_t ret5 = fds_record_delete(&record_desc_5);
          if (ret5 != FDS_SUCCESS)
          {
             printf("FDS Delete error \r\n");
          }

          printf("Reset OK \r\n");
        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+TIMEOUT", (size_t)10)) {
            
            char buf[100];
            strcpy(buf, incoming_message.data);
            char *uuid_char = strtok(buf, " ");
            uuid_char = strtok(NULL, " ");
            uint32_t timeout = atoi(uuid_char);
            //printf("%d \r\n", timeout);
            
            if (timeout < 0) {
              printf("Timeout cannot be negative \r\n");
            }
            else {
              writeFlashID(timeout, 5);
              time_out = timeout; 

              printf("OK \r\n");
            }
        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+TXPOWER", (size_t)10)) {
            
            char buf[100];
            strcpy(buf, incoming_message.data);
            char *uuid_char = strtok(buf, " ");
            uuid_char = strtok(NULL, " ");
            uint32_t tx_power = atoi(uuid_char);
            //printf("%d \r\n", timeout);
            
            if (tx_power < 0 || tx_power > 1) {
              printf("Tx Power parameter input error \r\n");
            }
            else {
              writeFlashID(tx_power, 6);
              if (tx_power == 0) {
                config_tx.power = TX_POWER_MAN_DEFAULT;
              }
              if (tx_power == 1) {
                config_tx.power = TX_POWER_MAX;
              }
              dwt_configuretxrf(&config_tx);
              printf("OK \r\n");
            }
        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+STREAMMODE", (size_t)13)) {
            
            char buf[100];
            strcpy(buf, incoming_message.data);
            char *uuid_char = strtok(buf, " ");
            uuid_char = strtok(NULL, " ");
            uint32_t stream_mode = atoi(uuid_char);
            
            if (stream_mode < 0 || stream_mode > 1) {
              printf("Stream mode parameter input error \r\n");
            }
            else {
              writeFlashID(stream_mode, 7);
              streaming_mode = stream_mode;
              printf("OK \r\n");
            }
        }

        else if (0 == strncmp((const char *)incoming_message.data, (const char *)"AT+LIST", (size_t)7)) {
            //print_seen_list();
        }

        else printf("ERROR Invalid AT Command\r\n");
      }

      else if(0 == strncmp((const char *)incoming_message.data, (const char *)"AT", (size_t)2))
      {
        printf("Only input AT without + command \r\n");
      }

      else {
        printf("Not an AT command\r\n");
        
      }  
    }
    if (debug_print == 1) printf("uart task out \r\n");
  }
}


/**
 * @brief UWB ranging task.
 */
void ranging_task_function(void *pvParameter)
{
  
  while(1){
    
      if(initiator_freq != 0)
      {
        uint16_t rand = get_rand_num(initiator_freq);
        vTaskDelay(rand);
        //printf("rand: %d \r\n", rand);
        //vTaskDelay(initiator_freq);
        if (debug_print == 1) printf("ranging task in \r\n");
        
        xSemaphoreTake(sus_resp, 0); //Suspend Responder Task
        xSemaphoreTake(sus_init, portMAX_DELAY);


        vTaskDelay(2);

        dwt_forcetrxoff();
        init_reconfig();

        int i = 0;
     
        while(i < MAX_ANCHOR_COUNT) {

          if (seen_list[i].UUID != 0) {

            if (debug_print)printf("IN\r\n");
            //printf("range 1 time keeper: %d \r\n", time_keeper);
            float range1 = ss_init_run(seen_list[i].UUID);
            //printf("range 1 time keeper: %d \r\n", time_keeper);
            if (debug_print)printf("range 1 out \r\n");
            if (debug_print)printf("range 1: %f \r\n", range1);
            //vTaskDelay(10);
            //printf("range 2 time keeper: %d \r\n", time_keeper);
            //float range2 = ss_init_run(seen_list[i].UUID);
            //printf("range 2 time keeper: %d \r\n", time_keeper);
            //if (debug_print)printf("range 2 out \r\n");
            //if (debug_print)printf("range 2: %f \r\n", range2);
            //vTaskDelay(10);
            //printf("range 3 time keeper: %d \r\n", time_keeper);
            //float range3 = ss_init_run(seen_list[i].UUID);
            //printf("range 3 time keeper: %d \r\n", time_keeper);
            //if (debug_print)printf("range 3 out \r\n");
            //if (debug_print)printf("range 3: %f \r\n", range3);
            //vTaskDelay(10);

            float range2 = -1;
            float range3 = -1;
            if (debug_print)printf("OUT\r\n");

            int numThru = 3;
            if (range1 == -1) {
              range1 = 0;
              numThru -= 1;
            }

            if (range2 == -1) {
              range2 = 0;
              numThru -= 1;
            }

            if(range3 == -1) {
              range3 = 0;
              numThru -= 1;
            }
        
            //if( (numThru != 0) ) printf("%f \r\n", (range1 + range2 + range3)/numThru);
            float range = (range1 + range2 + range3)/numThru;
            
            if( (numThru != 0) && (range >= -5) && (range <= 100) ) {
              seen_list[i].update_flag = 1;
              seen_list[i].range = range;
              seen_list[i].time_stamp = time_keeper;
              printf("node: %d; range: %f; timestamp: %u \r\n",seen_list[i].UUID, seen_list[i].range, time_keeper);
            }
          }
          i++;     
        }
        
        resp_reconfig();
        dwt_forcetrxoff();
        
        xSemaphoreGive(sus_init);
        xSemaphoreGive(sus_resp); //Resume Responder Task

      }
      else {
        vTaskDelay(1000);
        if (debug_print == 1) printf("ranging task in \r\n");
      }
      if (debug_print == 1) printf("ranging task out \r\n");
    }

 }

/**
 * @brief function to check nodes eviction
 */
void monitor_task_function()
{

  uint32 count = 0;

  while(1) {

    vTaskDelay(1000);
    if (debug_print == 1) printf("monitor task in \r\n");

    count += 1;
    
    xSemaphoreTake(sus_init, portMAX_DELAY);
    int removed = 0;
    for(int x = 0; x < MAX_ANCHOR_COUNT; x++)
      {
        if(seen_list[x].UUID != 0)
        {
          if( (time_keeper - seen_list[x].time_stamp) >= time_out) //Timeout Eviction
          {
            removed = 1;
            seen_list[x].UUID = 0;
            seen_list[x].range = 0;
            seen_list[x].time_stamp = 0;
            seen_list[x].RSSI = 0;
            seen_list[x].update_flag = 0;
          }
        }
      }
      
      if(removed || node_added || ((count % 5) == 0)  ) //Re-sort by RSSI
      {
        //Now sort neighbor list
        (void) sd_ble_gap_scan_stop();

        for (int j = 0; j < MAX_ANCHOR_COUNT; j++)
        {
          for( int k = j+1; k < MAX_ANCHOR_COUNT; k++)
          {
            if(seen_list[j].RSSI < seen_list[k].RSSI)
            {
                //printf("j : %d k: %d \r\n", seen_list[j].RSSI, seen_list[k].RSSI);
                node A = seen_list[j];
                seen_list[j] = seen_list[k];
                seen_list[k] = A;
         
            }
          }
        }

        scan_start(); //Resume scanning/building up neighbor list
        node_added = 0;
        removed = 0;
        count = 0;
      }
      
      xSemaphoreGive(sus_init);


    //if(debug_print)printf("Still alive \r\n");
    if (debug_print == 1) printf("monitor task out \r\n");
  }
}



/**
 * @brief program main entrance.
 */
int main(void)
{

    debug_print = 0;
    streaming_mode = 0;
    bool erase_bonds;
    int count = 0;

    // Init nodes in seen list
    for(int i = 0; i < MAX_ANCHOR_COUNT; i++)
    {
      seen_list[i].UUID = 0;
      seen_list[i].RSSI = 0;
      seen_list[i].time_stamp = 0;
      seen_list[i].range = 0;
      seen_list[i].update_flag = 0;
    }
  
    uart_init();
    timer_init();
    buttons_leds_init(&erase_bonds);
    ble_stack_init();
    gap_params_init();
    gatt_init();
    conn_params_init();
    peer_manager_init();
    advertising_init();

    // Init flash data storage
    ret_code_t ret = fds_register(fds_evt_handler);
    if (ret != FDS_SUCCESS)
    {
        printf("reg error \r\n"); // Registering of the FDS event handler has failed.
    }
    ret = fds_init();
    if (ret != FDS_SUCCESS)
    {
       printf("init error \r\n");
    }
    
    // Logic analyzer debug part
    nrf_gpio_cfg_output(12);
    nrf_gpio_cfg_output(27);

    // UWB config part
    nrf_gpio_cfg_input(DW1000_IRQ, NRF_GPIO_PIN_NOPULL); 	
    
    /* Reset DW1000 */
    reset_DW1000(); 

    /* Set SPI clock to 2MHz */
    port_set_dw1000_slowrate();			
  
    /* Init the DW1000 */
    if (dwt_initialise(DWT_LOADUCODE) == DWT_ERROR)
    {
      //Init of DW1000 Failed
      while (1) {};
    }

    // Set SPI to 8MHz clock  
    port_set_dw1000_fastrate();

    /* Configure DW1000. */
    dwt_configure(&config);

    /* Configure DW1000 TX power and pulse delay */
    dwt_configuretxrf(&config_tx);

    /* Initialization of the DW1000 interrupt*/

    //dwt_setcallbacks(&tx_conf_cb, &rx_ok_cb, &rx_to_cb, &rx_err_cb);

    /* Enable wanted interrupts (TX confirmation, RX good frames, RX timeouts and RX errors). */
    //dwt_setinterrupt(DWT_INT_TFRS | DWT_INT_RFCG | DWT_INT_RFTO | DWT_INT_RXPTO | DWT_INT_RPHE | DWT_INT_RFCE | DWT_INT_RFSL | DWT_INT_SFDT, 1);

    /* Apply default antenna delay value. See NOTE 2 below. */
    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);

    /* Set preamble timeout for expected frames. See NOTE 3 below. */
    //dwt_setpreambledetecttimeout(0); // PRE_TIMEOUT
          
    /* Set expected response's delay and timeout. */
    //dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
    dwt_setrxtimeout(0);

    // Init timekeeper from timer. unit: 1ms
    ret_code_t create_timer = app_timer_create(&m_timestamp_keeper, APP_TIMER_MODE_REPEATED, timestamp_handler);
    ret_code_t start_timer =  app_timer_start(m_timestamp_keeper, APP_TIMER_TICKS(1) , NULL);
  
    
    printf("Node On: Firmware version %s\r\n", FIRMWARE_VERSION);
    
    bsp_board_led_on(BSP_BOARD_LED_0);

 
    if (erase_bonds == true)
    {
        // Scanning and advertising is done upon PM_EVT_PEERS_DELETE_SUCCEEDED event.
        delete_bonds();
        // Scanning and advertising is done by
    }
    else
    {
        //adv_scan_start();
        //sd_ble_gap_adv_stop();
        //sd_ble_gap_scan_stop();
    }
    

    rxSemaphore = xSemaphoreCreateBinary();
    txSemaphore = xSemaphoreCreateBinary();
    sus_resp = xSemaphoreCreateBinary();
    sus_init = xSemaphoreCreateBinary();
    print_list_sem = xSemaphoreCreateBinary();
    //xSemaphoreGive(sus_resp);

    uart_queue = xQueueCreate(25, sizeof(struct message));

    UNUSED_VARIABLE(xTaskCreate(ss_responder_task_function, "SSTWR_RESP", configMINIMAL_STACK_SIZE+600, NULL,1, &ss_responder_task_handle));
    UNUSED_VARIABLE(xTaskCreate(ranging_task_function, "RNG", configMINIMAL_STACK_SIZE+200, NULL, 2, &ranging_task_handle));
    UNUSED_VARIABLE(xTaskCreate(uart_task_function, "UART", configMINIMAL_STACK_SIZE+1200, NULL, 2, &uart_task_handle));
    UNUSED_VARIABLE(xTaskCreate(list_task_function, "LIST", configMINIMAL_STACK_SIZE+600, NULL, 2, &list_task_handle));
    UNUSED_VARIABLE(xTaskCreate(monitor_task_function, "MONITOR", configMINIMAL_STACK_SIZE+800, NULL, 2, &monitor_task_handle));
    //UNUSED_VARIABLE(xTaskCreate(ble_task_fuction, "BLE", configMINIMAL_STACK_SIZE+1600, NULL, 0, &ble_task_handle));
    

    printf("Flash Configuration: \r\n");
    fds_record_desc_t   record_desc_2;
    fds_find_token_t    ftok_2;
    memset(&ftok_2, 0x00, sizeof(fds_find_token_t));
    if (fds_record_find(FILE_ID, RECORD_KEY_1, &record_desc_2, &ftok_2) == FDS_SUCCESS) //If there is a stored ID
    {
        
        uint32_t id = getFlashID(1); //Get ID
        NODE_UUID = id;
        m_adv_uuids[1].uuid = NODE_UUID; 
        advertising_init(); //Set UUID
        printf("  Node ID: %d \r\n", id);

        uint32_t state = getFlashID(2); //Get State

        printf("  Boot Mode: %d \r\n", state);

        if( (state == 1) || (state == 2) ) //Start BLE
        {
          ble_started = 1;
          xSemaphoreGive(print_list_sem);
          adv_scan_start();
          bsp_board_led_on(BSP_BOARD_LED_1);
        }

        if (state == 2) //Start UWB
        {
          uwb_started = 1;
          xSemaphoreGive(sus_resp);
          xSemaphoreGive(sus_init);
          bsp_board_led_on(BSP_BOARD_LED_2);
        }

        //printf("%d %d\r\n", id, state);

    }

    /* Fetch rate record from flash */
    fds_record_desc_t   record_desc_3;
    fds_find_token_t    ftok_3;
    memset(&ftok_3, 0x00, sizeof(fds_find_token_t));
    if (fds_record_find(FILE_ID, RECORD_KEY_3, &record_desc_3, &ftok_3) == FDS_SUCCESS) //If there is a stored rate
    {
      uint32_t rate = getFlashID(3);

      initiator_freq = rate;
      printf("  UWB Polling Rate: %d\r\n", rate);
    }

    /* Fetch channel record from flash */
    fds_record_desc_t   record_desc_4;
    fds_find_token_t    ftok_4;
    memset(&ftok_4, 0x00, sizeof(fds_find_token_t));
    if (fds_record_find(FILE_ID, RECORD_KEY_4, &record_desc_4, &ftok_4) == FDS_SUCCESS) //If there is a stored channel
    {
      uint32_t channel = getFlashID(4);

      config.chan = channel;
      dwt_configure(&config);
      printf("  UWB Channel: %d \r\n", channel);
    }

    /* Fetch timeout record from flash */
    fds_record_desc_t   record_desc_5;
    fds_find_token_t    ftok_5;
    memset(&ftok_5, 0x00, sizeof(fds_find_token_t));
    if (fds_record_find(FILE_ID, RECORD_KEY_5, &record_desc_5, &ftok_5) == FDS_SUCCESS) //If there is a stored channel
    {
      uint32_t timeout = getFlashID(5);
      //printf("fetch flash timeout: %d \r\n", timeout);

      time_out = timeout;
      printf("  BLE Timeout: %d \r\n", time_out);
    }

    /* Fetch TX power record from flash */
    fds_record_desc_t   record_desc_6;
    fds_find_token_t    ftok_6;
    memset(&ftok_6, 0x00, sizeof(fds_find_token_t));
    if (fds_record_find(FILE_ID, RECORD_KEY_6, &record_desc_6, &ftok_6) == FDS_SUCCESS) //If there is a stored channel
    {
      uint32_t tx_power = getFlashID(6);
      //printf("fetch flash timeout: %d \r\n", timeout);
      if (tx_power == 1) {
        config_tx.power = TX_POWER_MAX;
        printf("  TX Power: Max \r\n");
      }
      if (tx_power == 0) {
        config_tx.power = TX_POWER_MAN_DEFAULT;
        printf("  TX Power: Default \r\n");
      }
      dwt_configuretxrf(&config_tx);
    }

    /* Fetch streaming mode from flash */
    fds_record_desc_t   record_desc_7;
    fds_find_token_t    ftok_7;
    memset(&ftok_7, 0x00, sizeof(fds_find_token_t));
    if (fds_record_find(FILE_ID, RECORD_KEY_7, &record_desc_7, &ftok_7) == FDS_SUCCESS) //If there is a stored channel
    {
      uint32_t stream_mode = getFlashID(7);

      streaming_mode = stream_mode;
      printf("  Stream Mode: %d \r\n", stream_mode);
    }

   
    vTaskStartScheduler();
    while(1) 
    {

        APP_ERROR_HANDLER(NRF_ERROR_FORBIDDEN);
        
        
    }
}





