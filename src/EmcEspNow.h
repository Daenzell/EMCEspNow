/*
 * EmcEspNow.h
 * Author: Daenzell
 * Modified by: [ebolzMagy]
 * Simplified EMC ESP-NOW communication with peer detection
 */

#if defined(ESP32)
#pragma once

#include <esp_now.h>
#include <WiFi.h>

#define ESPNOW_WIFI_CHANNEL 11
#define MAX_SLAVES 5               // Maximum number of slaves that can be connected to the master
#define PEER_TIMEOUT_MS 3000       // Peer detection timeout (3 seconds)
#define HEARTBEAT_INTERVAL_MS 1000 // Heartbeat interval (1 second)


// Data structure that the slave sends to the master
typedef struct
{
    uint8_t button_data[16] = {0};
    uint8_t data[64] = {0};
    uint32_t sequence = 0; // For detecting data changes
} __attribute__((packed)) slave_data_t;

// Command structure from master to slave
enum CMDType : uint8_t
{
    CMD_SET,
    CMD_GET
};

typedef struct
{
    uint8_t mainId = 0;
    uint8_t subId = 0;
    uint8_t index1 = 0;
    uint8_t index2 = 0;
    float value = 0;
    int32_t valueInt = 0;
    uint32_t sequence = 0; // For detecting data changes
} __attribute__((packed)) master_cmd_t;

class EmcEspNow
{
public:
    void begin(bool isMaster);
    void end();
    uint8_t getWiFiChannel() const;
    uint8_t getWiFiMode() const;

    void addPeer(const uint8_t *peer_addr);
    void removePeer(const uint8_t *peer_mac);
    void sendBroadcast();
    void sendUnicast(const uint8_t *peer_mac, const uint8_t *data, size_t len);
    void update();
    void onReceive(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
    void resetData();

    // Public data structures
    slave_data_t slaveSendData;     // Data sent by slave
    slave_data_t masterRecvData;    // Last data received by master
    master_cmd_t masterCmdData;     // Command to be sent by master
    master_cmd_t lastmasterCmdData; // Previous command

    // Connection status functions
    bool isMasterConnected() const { return masterConnected; }
    bool isSlaveConnected(uint8_t index) const;

    int getConnectedSlaveCount() const;
    const uint8_t *getSlaveMac(int index) const;
private:
    // For slave: stores master information
    uint8_t masterMac[6] = {0};
    unsigned long lastMasterRxTime = 0;
    bool masterConnected = false;

    // For master: stores list of slaves
    struct SlaveInfo
    {
        uint8_t mac[6];
        unsigned long lastRxTime;
        bool active;
    };
    SlaveInfo slaves[MAX_SLAVES];
    int slaveCount = 0;

    slave_data_t lastSlaveSendData;
    unsigned long broadcastMillis = 0;
    unsigned long lastHeartbeatSend = 0;

    const char *BROADCAST_SLAVE_MESSAGE = "EMCFFBV2 Slave!";
    const char *BROADCAST_MASTER_MESSAGE = "EMCFFBV2 Master!";

    const uint8_t BROADCAST_MAC_SLAVE[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD};
    const uint8_t BROADCAST_MAC_MASTER[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};

    bool isMaster = false;

    static EmcEspNow *instance;
    static void onSend(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);
    static void espNowTask(void *pvParameters);

    // Helper to find slaves based on MAC
    int findSlaveIndex(const uint8_t *mac);
    // Add or update slave based on received MAC
    void updateSlave(const uint8_t *mac);
    // Check for slave timeouts and mark as inactive if timed out
    void checkSlaveTimeouts();
    // Send heartbeat if needed (for slave to master)
    void sendHeartbeatIfNeeded();

    // Channel scanning variables
    uint8_t currentChannel = 1;
    
};

#endif