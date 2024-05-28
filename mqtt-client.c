/*
 * Copyright (c) 2020, Carlo Vallati, University of Pisa
  * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*---------------------------------------------------------------------------*/
#include "contiki.h"
#include "net/routing/routing.h"
#include "mqtt.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-icmp6.h"
#include "net/ipv6/sicslowpan.h"
#include "sys/etimer.h"
#include "sys/ctimer.h"
#include "lib/sensors.h"
#include "dev/button-hal.h"
#include "dev/leds.h"
#include "os/sys/log.h"
#include "mqtt-client.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "net/netstack.h"


/*---------------------------------------------------------------------------*/
#define LOG_MODULE "mqtt-client"
#ifdef MQTT_CLIENT_CONF_LOG_LEVEL
#define LOG_LEVEL MQTT_CLIENT_CONF_LOG_LEVEL
#else
#define LOG_LEVEL LOG_LEVEL_DBG
#endif

/*---------------------------------------------------------------------------*/
/* MQTT broker address. */
#define MQTT_CLIENT_BROKER_IP_ADDR "fd00::1"

static const char *broker_ip = MQTT_CLIENT_BROKER_IP_ADDR;

// Defaukt config values
#define DEFAULT_BROKER_PORT         1883
#define DEFAULT_PUBLISH_INTERVAL    (30 * CLOCK_SECOND)


// We assume that the broker does not require authentication


/*---------------------------------------------------------------------------*/
/* Various states */
static uint8_t state;

#define STATE_INIT    		  0
#define STATE_NET_OK    	  1
#define STATE_CONNECTING      2
#define STATE_CONNECTED       3
#define STATE_SUBSCRIBED      4
#define STATE_DISCONNECTED    5

/*---------------------------------------------------------------------------*/
PROCESS_NAME(mqtt_client_process);
AUTOSTART_PROCESSES(&mqtt_client_process);

/*---------------------------------------------------------------------------*/
/* Maximum TCP segment size for outgoing segments of our socket */
#define MAX_TCP_SEGMENT_SIZE    32
#define CONFIG_IP_ADDR_STR_LEN   64
/*---------------------------------------------------------------------------*/
/*
 * Buffers for Client ID and Topics.
 * Make sure they are large enough to hold the entire respective string
 */
#define BUFFER_SIZE 64

static char client_id[BUFFER_SIZE];
static char pub_topic[BUFFER_SIZE];
static char sub_topic[BUFFER_SIZE];

#define NUM_SENSORS 4
#define PUBLISH_COUNT 10


// Periodic timer to check the state of the MQTT client
#define STATE_MACHINE_PERIODIC     (CLOCK_SECOND >> 1)
static struct etimer periodic_timer;

/*---------------------------------------------------------------------------*/
/*
 * The main MQTT buffers.
 * We will need to increase if we start publishing more data.
 */
#define APP_BUFFER_SIZE 512
#define BUFFER_SIZE 64
#define JSON_BUFFER_SIZE 256
//static char app_buffer[APP_BUFFER_SIZE];
/*---------------------------------------------------------------------------*/
static struct mqtt_message *msg_ptr = 0;

static struct mqtt_connection conn;

// Variable to store the status of the last MQTT operation. This status can be 
// used to check if an operation like connecting, disconnecting, publishing, 
// or subscribing was successful.

mqtt_status_t status;
char broker_address[CONFIG_IP_ADDR_STR_LEN];


/*---------------------------------------------------------------------------*/
PROCESS(mqtt_client_process, "MQTT Client");


/*---------------------------------------------------------------------------*/
/*============================================================================*/
//Simulated thresholds for each sensor
double thresholds[NUM_SENSORS] = {10, 0.1, 15, 12};  //temperature, humidity, CO, and light


//double thresholds[NUM_SENSORS] = {1.05, .99, 1.01, 1.05};  //temperature, humidity, CO, and light

void get_sensor_readings(int *readings) {
    for (int i = 0; i < NUM_SENSORS; i++) {
        readings[i] = (int)(rand() % 100 + 1); // Random sensor values between 1 and 100
    }
}

static void
pub_handler(const char *topic, uint16_t topic_len, const uint8_t *chunk,
            uint16_t chunk_len)
{
  printf("Pub Handler: topic='%s' (len=%u), chunk_len=%u\n", topic, topic_len, chunk_len);
  if(strcmp(topic, "actuator") == 0) {
    printf("Received Actuator command\n");
	printf("%s\n", chunk);
    // Do something :)
    return;
  }
}
/*---------------------------------------------------------------------------*/

static void
mqtt_event(struct mqtt_connection *m, mqtt_event_t event, void *data)
{
  switch(event) {
  case MQTT_EVENT_CONNECTED: {
    printf("Application has a MQTT connection\n");
    state = STATE_CONNECTED;
    break;
  }
  case MQTT_EVENT_DISCONNECTED: {
    printf("MQTT Disconnect. Reason %u\n", *((mqtt_event_t *)data));
    state = STATE_DISCONNECTED;
    process_poll(&mqtt_client_process);
    break;
  }
  case MQTT_EVENT_PUBLISH: {
    msg_ptr = data;
    pub_handler(msg_ptr->topic, strlen(msg_ptr->topic),
                msg_ptr->payload_chunk, msg_ptr->payload_length);
    break;
  }
  case MQTT_EVENT_SUBACK: {
#if MQTT_311
    mqtt_suback_event_t *suback_event = (mqtt_suback_event_t *)data;
    if(suback_event->success) {
      printf("Application is subscribed to topic successfully\n");
    } else {
      printf("Application failed to subscribe to topic (ret code %x)\n", suback_event->return_code);
    }
#else
    printf("Application is subscribed to topic successfully\n");
#endif
    break;
  }
  case MQTT_EVENT_UNSUBACK: {
    printf("Application is unsubscribed to topic successfully\n");
    break;
  }
  case MQTT_EVENT_PUBACK: {
    printf("Publishing complete.\n");
    break;
  }
  default:
    printf("Application got a unhandled MQTT event: %i\n", event);
    break;
  }
}

static bool
have_connectivity(void)
{
  if(uip_ds6_get_global(ADDR_PREFERRED) == NULL ||
     uip_ds6_defrt_choose() == NULL) {
    return false;
  }
  return true;
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(mqtt_client_process, ev, data)
{
static struct etimer et;
  PROCESS_BEGIN();
  printf("MQTT Client Process\n");
  // Initialize the ClientID as MAC address
  snprintf(client_id, BUFFER_SIZE, "%02x%02x%02x%02x%02x%02x",
                     linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1],
                     linkaddr_node_addr.u8[2], linkaddr_node_addr.u8[5],
                     linkaddr_node_addr.u8[6], linkaddr_node_addr.u8[7]);

  // Broker registration					 
  mqtt_register(&conn, &mqtt_client_process, client_id, mqtt_event,
                  MAX_TCP_SEGMENT_SIZE);
				  
  state=STATE_INIT;
				    
  // Initialize periodic timer to check the status 
  etimer_set(&periodic_timer, STATE_MACHINE_PERIODIC);

  /* Main loop */
  while(1) {

    PROCESS_YIELD();

    if((ev == PROCESS_EVENT_TIMER && data == &periodic_timer) || 
	      ev == PROCESS_EVENT_POLL){			  			  
		  if(state==STATE_INIT){
			 if(have_connectivity()==true)  
				 state = STATE_NET_OK;
		  } 		  
		  if(state == STATE_NET_OK){
			  // Connect to MQTT server
			  printf("Connecting!\n");
			  memcpy(broker_address, broker_ip, strlen(broker_ip));
			  mqtt_connect(&conn, broker_address, DEFAULT_BROKER_PORT,
						   (DEFAULT_PUBLISH_INTERVAL * 3) / CLOCK_SECOND,
						   MQTT_CLEAN_SESSION_ON);
			  state = STATE_CONNECTING;
		  }  
		  if(state==STATE_CONNECTED){	  
			  // Subscribe to a topic
			  strcpy(sub_topic,"actuator");

			  status = mqtt_subscribe(&conn, NULL, sub_topic, MQTT_QOS_LEVEL_0);

			  printf("Subscribing!\n");
			  if(status == MQTT_STATUS_OUT_QUEUE_FULL) {
				LOG_ERR("Tried to subscribe but command queue was full!\n");
				PROCESS_EXIT();
			  }
			  
			  state = STATE_SUBSCRIBED;
	       }	  
		if(state == STATE_SUBSCRIBED){
		    static int current_readings[NUM_SENSORS];
		    static int previous_readings[NUM_SENSORS] = {0};
		    static double ratios[NUM_SENSORS];
		    static int counts[NUM_SENSORS] = {0};// Counters for each sensor
		    static int publish_mode[NUM_SENSORS] = {0};// Flags to check if sensor is in publishing mode
		    static int publish_counts[NUM_SENSORS] = {0}; // Counts for number of publishes
		     		   	   
		   // Initialize previous readings
		  get_sensor_readings(previous_readings);
		 while (1) 
		  {
		  get_sensor_readings(current_readings);
		  for (int i = 0; i < NUM_SENSORS; i++) {
		    if (previous_readings[i] == 0) {
		      ratios[i] = (current_readings[i] +1) / (previous_readings[i] + 1);
				} 
		     else { ratios[i] = current_readings[i] / previous_readings[i]; }

		     if (publish_mode[i]) {
			// Publish data directly if in publishing mode
			//double temperature_reading = current_readings[0];    
			//double humidity_reading = current_readings[1];        
			//double co_reading = current_readings[2];              
			//double light_intensity_reading = current_readings[3]; 
			//Generate Hardware-Based Client ID 
    			char client_id[BUFFER_SIZE]; 
    			uint8_t* mac = linkaddr_node_addr.u8; 
    			snprintf(client_id, BUFFER_SIZE, "%02x%02x%02x%02x%02x%02x", 
            		         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
			sprintf(pub_topic, "%s", "Data");
			char json_payload[JSON_BUFFER_SIZE];
			printf("Readings: %d, %d, %d, %d\n", current_readings[0], current_readings[1], current_readings[2], current_readings[3]);
			   snprintf(json_payload, sizeof(json_payload),"{\"sensor_id\":\"%s\", \"readings\":[%d, %d, %d, %d]}",
             client_id, current_readings[0], current_readings[1], current_readings[2], current_readings[3]);             
			// Publish the correctly formatted JSON stored in json_payload
			mqtt_publish(&conn, NULL, pub_topic, (uint8_t *)json_payload,
				     strlen(json_payload), MQTT_QOS_LEVEL_0, MQTT_RETAIN_OFF);
		 	publish_counts[i]++;
			if (publish_counts[i] >= PUBLISH_COUNT) {
			   publish_mode[i] = 0; // Turn off publish mode
			   publish_counts[i] = 0; // Reset publish count
				}
			 } 
			 else {
			   if (((i == 0 || i == 3 || i == 2) && ratios[i] >= thresholds[i]) || ((i == 1) && ratios[i] <= thresholds[i])){
				counts[i]++;
			     } 
			     else { counts[i] = 0;}
			     
			     if (counts[i] == 3) {
				publish_mode[i] = 1; // Start publish mode
				counts[i] = 0; // Reset count
				}
			    }
			    previous_readings[i] = current_readings[i];
			}

		    // Delay for 10 seconds   
		    etimer_set(&et, CLOCK_SECOND * 10);
		    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
		    }
                   } else if ( state == STATE_DISCONNECTED ){
		   LOG_ERR("Disconnected form MQTT broker\n");	
		   // Recover from error
		   mqtt_disconnect(&conn);
		   /* If disconnection occurs the state is changed to STATE_INIT in this way a new connection attempt starts */
		   state=STATE_INIT;
		}
				
		etimer_set(&periodic_timer, STATE_MACHINE_PERIODIC);
    	}

    }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
