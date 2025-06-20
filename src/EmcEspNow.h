#if defined(ESP32)
#pragma once

#include <esp_now.h>
#include <WiFi.h>
#define ESPNOW_WIFI_CHANNEL 6

typedef struct
{

    uint8_t button_data[16] = {0};
    uint8_t data[64] = {0};
} __attribute__((packed)) slave_data_t;

enum CMDType : uint8_t
{
    CMD_SET,
    CMD_GET
};

class EmcEspNow
{
    typedef struct
    {
        uint8_t mainId = 0;
        uint8_t subId = 0;
        uint8_t index1 = 0;
        uint8_t index2 = 0;
        float value = 0;
        int32_t valueInt = 0;
    } __attribute__((packed)) master_cmd_t;

    typedef struct
    {
        uint8_t peerID;
        uint8_t peer_mac[6];
    } __attribute__((packed)) peers_t;

public:
    void begin(bool isMaster);
    void addPeer(const uint8_t *peer_addr);
    void removePeer(const uint8_t *peer_mac);
    void sendBroadcast();
    void sendUnicast(const uint8_t *peer_mac, const uint8_t *data, size_t len);
    void update();
    void onReceive(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
    void resetData();

    slave_data_t slaveSendData;
    slave_data_t masterRecvData;
    
    master_cmd_t masterCmdData; 
    master_cmd_t lastmasterCmdData;

    std::vector<peers_t> peers;

private:
    slave_data_t lastSlaveSendData;
    

    unsigned long broadcastMillis = 0;

    const char *BROADCAST_SLAVE_MESSAGE = "EMCFFBV2 Slave!";
    const char *BROADCAST_MASTER_MESSAGE = "EMCFFBV2 Master!";
    /*BROADCAST MAC ADDRESS*/
    const uint8_t BROADCAST_MAC_SLAVE[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD};
    const uint8_t BROADCAST_MAC_MASTER[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};

    bool isMaster = false;
    bool masterHasCommand = false;
    
    static EmcEspNow *instance;

    static void onSend(const uint8_t *mac_addr, esp_now_send_status_t status);
    static void espNowTask(void *pvParameters);
};

#endif