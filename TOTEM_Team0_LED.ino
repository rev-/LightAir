
/*********
  Marce Ferra 2021
  Proyecto de emisor y receptor para tres entradas lógicas (para contacto seco) en emisor y tres salidas lógicas de estado en receptor
  Usando protocolo ESP NOW de Espressif
  MODULO RECEPTOR con intento de ver el RSSI del paquete recibido. Creo...
  
  Si esta información te resulta útil e interesante, invitame un cafecito!!!
  https://cafecito.app/marce_ferra

  Desde fuera de Argentina en:
  https://www.buymeacoffee.com/marceferra

  If you found this information useful and interesting, buy me a cafecito!!!
  https://www.buymeacoffee.com/marceferra
*********/


#include "esp_wifi.h"
#include <esp_now.h>
#include <WiFi.h>
#include <FastLED.h>

#define CHANNEL 1
#define MYTEAM 0    // Team
#define TEAMNUM 2
#define RSSI_LIMIT -50

#define DATA_PIN    13
#define NUM_LEDS    13
#define BRIGHTNESS  255
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

CRGB leds[NUM_LEDS];

#define BUTTON_R 4
#define BUTTON_G 19
#define BUTTON_B 21
#define BUTTON_COMM 18

int lasttime = 0;
int blinktime = 2000;
bool blink = false;

uint8_t r,g,b = 0;

int rssi_display=-1000;
int last_rssi=-1000;
int siglen = 0;
uint8_t sendaddr[6];
uint8_t incomingMsg[500];
int msglen = 0;
boolean rdy = false;
boolean newmsg = false;

uint8_t newMAC[] = {0x1A, 0x17, 0xA1, 0x00, 0x01, MYTEAM};
//                  0x1A  0x17  0xA1  ?     Role  mynumber
//                  Role: 01 = TOTEM ; 00 = proj

esp_now_peer_info_t peer[1] = {};

// Estructuras para calcular los paquetes, el RSSI, etc
typedef struct {
  unsigned frame_ctrl: 16;
  unsigned duration_id: 16;
  uint8_t addr1[6]; /* receiver address */
  uint8_t addr2[6]; /* sender address */
  uint8_t addr3[6]; /* filtering address */
  unsigned sequence_ctrl: 16;
  uint8_t addr4[6]; /* optional */
} wifi_ieee80211_mac_hdr_t;

typedef struct {
  wifi_ieee80211_mac_hdr_t hdr;
  uint8_t payload[0]; /* network data ended with 4 bytes csum (CRC32) */
} wifi_ieee80211_packet_t;

void sendData( uint8_t data) {

  esp_err_t result = esp_now_send(peer[0].peer_addr, &data, sizeof(data));
//  const uint8_t *peer_addr = peer[targetnum].peer_addr;
//  // ESP_BT.print("Sending: "); ESP_BT.println(data);
//  esp_err_t result = esp_now_send(peer_addr, &data, sizeof(data));
  
//  // ESP_BT.print("Send Status: ");
//  if (result == ESP_OK) {
//   //sentsuccess=true;
//  } else if (result == ESP_ERR_ESPNOW_NOT_INIT) {
//    // How did we get so far!!
//    // ESP_BT.println("ESPNOW not Init.");
//  } else if (result == ESP_ERR_ESPNOW_ARG) {
//    // ESP_BT.println("Invalid Argument");
//  } else if (result == ESP_ERR_ESPNOW_INTERNAL) {
//    // ESP_BT.println("Internal Error");
//  } else if (result == ESP_ERR_ESPNOW_NO_MEM) {
//    // ESP_BT.println("ESP_ERR_ESPNOW_NO_MEM");
//  } else if (result == ESP_ERR_ESPNOW_NOT_FOUND) {
//    // ESP_BT.println("Peer not found.");
//  } else {
//    // ESP_BT.println("Not sure what happened");
}


void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
//  if(debug){
//    char macStr[18];
//    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
//             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
//     Serial.print("Last Packet Sent to: "); Serial.println(macStr);
//     Serial.print("Last Packet Send Status: "); Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
//  }
}

void addBroadCast() {
  // memset(&peer, 0, sizeof(peer)); // clear slaves
  for (int ii = 0; ii<6; ii++){
    peer[0].peer_addr[ii] = (uint8_t)0xff;  
  }  
  peer[0].channel = CHANNEL;
  peer[0].encrypt = 0;

  manageSlave();
  
}

// Check if the slave is already paired with the master.
// If not, pair the slave with master
void manageSlave() {
      const esp_now_peer_info_t *peertry = &peer[0];
      const uint8_t *peertry_addr = peer[0].peer_addr;
      // ESP_BT.print("Processing: ");
      // for (int ii = 0; ii < 6; ++ii ) {
      //  ESP_BT.print((uint8_t) peertry_addr[ii], HEX);
      //  if (ii != 5) ESP_BT.print(":");
      // }
      // ESP_BT.print(" Status: ");
      // check if the peer exists
      bool exists = esp_now_is_peer_exist(peertry_addr);
      if (exists) {
        // Slave already paired.
        // ESP_BT.println("Already Paired");
      } else {
        // Slave not paired, attempt pair
        esp_err_t addStatus = esp_now_add_peer(peertry);
        if (addStatus == ESP_OK) {
          // Pair success
          // ESP_BT.println("Pair success");
        } else if (addStatus == ESP_ERR_ESPNOW_NOT_INIT) {
          // How did we get so far!!
          // ESP_BT.println("ESPNOW Not Init");
        } else if (addStatus == ESP_ERR_ESPNOW_ARG) {
          // ESP_BT.println("Add Peer - Invalid Argument");
        } else if (addStatus == ESP_ERR_ESPNOW_FULL) {
          // ESP_BT.println("Peer list full");
        } else if (addStatus == ESP_ERR_ESPNOW_NO_MEM) {
          // ESP_BT.println("Out of memory");
        } else if (addStatus == ESP_ERR_ESPNOW_EXIST) {
          // ESP_BT.println("Peer Exists");
        } else {
          // ESP_BT.println("Not sure what happened");
        }
      }
}


// Callback with trick for rssi: https://github.com/gmag11/QuickESPNow/blob/main/src/QuickEspNow_esp32.cpp 
void OnDataRecv (const uint8_t* mac_addr, const uint8_t* data, int len) {
    //espnow_frame_format_t* espnow_data = (espnow_frame_format_t*)(data - sizeof (espnow_frame_format_t));
    wifi_promiscuous_pkt_t* promiscuous_pkt = (wifi_promiscuous_pkt_t*)(data - sizeof (wifi_pkt_rx_ctrl_t) - 39); // 39 = sizeof (espnow_frame_format_t)
    wifi_pkt_rx_ctrl_t* rx_ctrl = &promiscuous_pkt->rx_ctrl;

    //comms_rx_queue_item_t message;

    //DEBUG_DBG (QESPNOW_TAG, "Received message with RSSI %d from " MACSTR " Len: %u", rx_ctrl->rssi, MAC2STR (mac_addr), len);

    //if to limit accepted packets by MAC and rssi value
    
    memcpy (sendaddr, mac_addr, 6);
    memcpy (incomingMsg, data, len);
    msglen = len;
    rssi_display = rx_ctrl->rssi;
    newmsg = true;
    
    //memcpy (message.dstAddress, espnow_data->destination_address, ESP_NOW_ETH_ALEN);

//    if (uxQueueMessagesWaiting (quickEspNow.rx_queue) >= quickEspNow.queueSize) {
//        comms_rx_queue_item_t tempBuffer;
//        xQueueReceive (quickEspNow.rx_queue, &tempBuffer, 0);
//        DEBUG_DBG (QESPNOW_TAG, "Rx Message dropped");
//    }
//#ifdef MEAS_TPUT
//    quickEspNow.rxDataReceived += len;
//#endif // MEAS_TPUT
//
//    if (!xQueueSend (quickEspNow.rx_queue, &message, pdMS_TO_TICKS (100))) {
//        DEBUG_WARN (QESPNOW_TAG, "Error sending message to queue");
//    }
}


void setup() {
  
  // Initialize Serial Monitor
  Serial.begin(115200);

  esp_base_mac_addr_set(newMAC);


  // Set device as a Wi-Fi Station
  WiFi.mode(WIFI_STA);

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  addBroadCast();
  // Once ESPNow is successfully Init, we will register for recv CB to
  // get recv packer info
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);

  pinMode(BUTTON_COMM, OUTPUT);
  pinMode(BUTTON_R, OUTPUT);
  pinMode(BUTTON_G, OUTPUT);
  pinMode(BUTTON_B, OUTPUT);
  digitalWrite(BUTTON_COMM, LOW);
  digitalWrite(BUTTON_R, LOW);
  digitalWrite(BUTTON_G, LOW);
  digitalWrite(BUTTON_B, LOW);

  for (int j = 0; j < NUM_LEDS; j++){
    leds[j]=CRGB(0,0,0);
  }
}

void loop() {


  if ((millis()-lasttime)>blinktime){ //toogle
    blink = !blink;
    digitalWrite(BUTTON_G, blink);
    digitalWrite(BUTTON_B, blink);
    lasttime = millis();
  }

  rdy = true;
  
  if( (newmsg) && (rssi_display > RSSI_LIMIT) ){       //&& (rssi_display > -35)
    // Packet qualified
    rdy = false;
    //Serial.println("Packet passed!");
    last_rssi = rssi_display;

    /*  Debug output
    Serial.print("RSSI: "); Serial.print(rssi_display); Serial.print("\t");
    Serial.print("From MAC");
    for (int i=0; i<6; i++){
      Serial.print(":"); 
      Serial.print(sendaddr[i],HEX);
    }  
    Serial.print("\t");
    Serial.print("Length: ");
    Serial.print(msglen);
    Serial.print("\t");
    Serial.print("Message: ");
    for (int k=0; k<msglen; k++){
      Serial.print(","); 
      Serial.print(incomingMsg[k],HEX);
    }  
    Serial.println(); 
    */

    // Send message to player n ( n = data[0]/15 from received message )
    // That message is simply: mymsg = data[0]/15
    // probably has to change mode, i.e. stop promiscuous mode 

    if (sendaddr[5]%TEAMNUM == MYTEAM){
    sendData(incomingMsg[0]/15);  // send respawn msg to teammates
      switch (sendaddr[5]) {
        case 1: // White
          r = 255; g = 255; b = 255;
          break;
        case 2: // Green
          r = 0; g = 255; b = 0;
          break;
        case 3: // Yellow
          r = 255; g = 255; b = 0;
          break;
        case 4: // Blue
          r = 0; g = 0; b = 255;
          break;
        case 5: // Orange
          r = 255; g = 100; b = 0;
        break;
        case 6: // Red
          r = 255; g = 0; b = 0;
        break;
        default:
          r = 0; g = 0; b = 0;
        break;
      }

      for (int i = 0; i <= NUM_LEDS; i++) {
          if (i<NUM_LEDS) leds[i]=CRGB(r,g,b);
          else leds[NUM_LEDS] = CRGB(0,0,0);
          if (i>0) leds[i-1] = CRGB(0,0,0); // CRGB(255, 0, 255);
        FastLED.show();
        delay(50);
      }
      delay(1000);

    }

    // Serial.print("Sent Data: "); Serial.println(incomingMsg[0]/15);
    memset(incomingMsg,0,sizeof(incomingMsg));

    newmsg = false;

  }
  // else Serial.println("Packet not passed");
  
}
