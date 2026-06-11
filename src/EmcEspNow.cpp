/*
 * EmcEspNow.cpp
 * Author: Daenzell
 * Modified by: [ebolzMagy]
 * Simplified ESP-NOW with peer detection
 */

#if defined(ESP32)

#include "EmcEspNow.h"
#include <cstring>

EmcEspNow *EmcEspNow::instance = nullptr;

void EmcEspNow::begin(bool isMaster)
{
    WiFi.mode(WIFI_STA);
    WiFi.setChannel(ESPNOW_WIFI_CHANNEL);

    if (esp_now_init() != ESP_OK)
    {
        log_e("Failed to initialize ESP-NOW");
        return;
    }

    esp_now_register_send_cb(onSend);
    esp_now_register_recv_cb([](const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
                             { if(instance) instance->onReceive(recv_info, data, len); });

    this->isMaster = isMaster;
    instance = this;
    resetData();

    // Register broadcast peers according to mode
    if (!isMaster)
    {
        addPeer(BROADCAST_MAC_SLAVE);
    }
    else
    {
        addPeer(BROADCAST_MAC_MASTER);
    }

    // Initialize slave list (for master)
    if (isMaster)
    {
        slaveCount = 0;
        for (int i = 0; i < MAX_SLAVES; i++)
        {
            slaves[i].active = false;
            memset(slaves[i].mac, 0, 6);
            slaves[i].lastRxTime = 0;
        }
    }
}

void EmcEspNow::end()
{
    resetData();
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();

    // Delete all listed peers (for master, delete all slaves)
    if (isMaster)
    {
        for (int i = 0; i < slaveCount; i++)
        {
            if (slaves[i].active)
                esp_now_del_peer(slaves[i].mac);
        }
    }
    else
    {
        if (masterConnected)
            esp_now_del_peer(masterMac);
    }

    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
    instance = nullptr;
}

uint8_t EmcEspNow::getWiFiChannel() const { return WiFi.channel(); }
uint8_t EmcEspNow::getWiFiMode() const { return WiFi.getMode(); }

void EmcEspNow::addPeer(const uint8_t *peer_addr)
{
    if (esp_now_is_peer_exist(peer_addr))
        return;

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(esp_now_peer_info_t));
    memcpy(peer.peer_addr, peer_addr, 6);
    peer.channel = ESPNOW_WIFI_CHANNEL;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) == ESP_OK)
    {
        log_d("Peer added: " MACSTR, MAC2STR(peer_addr));
    }
    else
    {
        log_e("Failed to add peer");
    }
}

void EmcEspNow::removePeer(const uint8_t *peer_mac)
{
    if (esp_now_is_peer_exist(peer_mac))
    {
        esp_now_del_peer(peer_mac);
        log_d("Peer removed: " MACSTR, MAC2STR(peer_mac));
    }
    // Remove from internal list
    if (isMaster)
    {
        int idx = findSlaveIndex(peer_mac);
        if (idx >= 0)
        {
            slaves[idx].active = false;
            slaveCount--;
        }

        // Reset masterRecvData if no active slaves
        bool anyActive = false;
        for (int i = 0; i < MAX_SLAVES; i++)
        {
            if (slaves[i].active)
            {
                anyActive = true;
                break;
            }
        }
        if (!anyActive)
        {
            memset(&masterRecvData, 0, sizeof(slave_data_t));
            log_d("No active slaves, masterRecvData reset");
        }
    }
    else
    {
        if (memcmp(masterMac, peer_mac, 6) == 0)
        {
            masterConnected = false;
            memset(masterMac, 0, 6);
            lastMasterRxTime = 0;
        }
    }
}

void EmcEspNow::sendBroadcast()
{
    if (!isMaster)
    {
        esp_now_send(BROADCAST_MAC_SLAVE, (uint8_t *)BROADCAST_SLAVE_MESSAGE, strlen(BROADCAST_SLAVE_MESSAGE));
    }
    else
    {
        esp_now_send(BROADCAST_MAC_MASTER, (uint8_t *)BROADCAST_MASTER_MESSAGE, strlen(BROADCAST_MASTER_MESSAGE));
    }
}

void EmcEspNow::sendUnicast(const uint8_t *peer_mac, const uint8_t *data, size_t len)
{
    esp_err_t result = esp_now_send(peer_mac, data, len);
    if (result != ESP_OK)
    {
        log_w("Send failed, removing peer");
        removePeer(peer_mac);
    }
}

void EmcEspNow::resetData()
{
    memset(&slaveSendData, 0, sizeof(slave_data_t));
    memset(&lastSlaveSendData, 0, sizeof(slave_data_t));
    memset(&masterRecvData, 0, sizeof(slave_data_t));
    memset(&masterCmdData, 0, sizeof(master_cmd_t));
    memset(&lastmasterCmdData, 0, sizeof(master_cmd_t));
}

// Helper: find slave by MAC
int EmcEspNow::findSlaveIndex(const uint8_t *mac)
{
    for (int i = 0; i < MAX_SLAVES; i++)
    {
        if (slaves[i].active && memcmp(slaves[i].mac, mac, 6) == 0)
            return i;
    }
    return -1;
}

// Add or update slave (activate and update timestamp)
void EmcEspNow::updateSlave(const uint8_t *mac)
{
    int idx = findSlaveIndex(mac);
    if (idx >= 0)
    {
        slaves[idx].lastRxTime = millis();
        return;
    }
    // Find an empty slot
    for (int i = 0; i < MAX_SLAVES; i++)
    {
        if (!slaves[i].active)
        {
            memcpy(slaves[i].mac, mac, 6);
            slaves[i].lastRxTime = millis();
            slaves[i].active = true;
            slaveCount++;
            // Add as an ESP-NOW peer if not already
            addPeer(mac);
            log_d("New slave connected: " MACSTR, MAC2STR(mac));
            return;
        }
    }
    log_w("Maximum slaves reached, cannot add " MACSTR, MAC2STR(mac));
}

int EmcEspNow::getConnectedSlaveCount() const {
    if (!isMaster) return 0;
    int count = 0;
    for (int i = 0; i < MAX_SLAVES; i++) {
        if (slaves[i].active) count++;
    }
    return count;
}

const uint8_t* EmcEspNow::getSlaveMac(int index) const {
    if (!isMaster || index < 0 || index >= MAX_SLAVES) return nullptr;
    return slaves[index].active ? slaves[index].mac : nullptr;
}

// Delete slaves that have timed out
void EmcEspNow::checkSlaveTimeouts()
{
    unsigned long now = millis();
    bool anyActive = false;
    for (int i = 0; i < MAX_SLAVES; i++)
    {
        if (slaves[i].active && (now - slaves[i].lastRxTime) > PEER_TIMEOUT_MS)
        {
            log_d("Slave " MACSTR " timeout, removing", MAC2STR(slaves[i].mac));
            removePeer(slaves[i].mac);
            slaves[i].active = false;
            slaveCount--;
        }
        else
        {
            anyActive = true;
        }
    }

    // If there are no active slaves, reset the received data
    if (!anyActive && isMaster)
    {
        memset(&masterRecvData, 0, sizeof(slave_data_t));
        log_d("No active slaves, masterRecvData reset");
    }
}

bool EmcEspNow::isSlaveConnected(uint8_t index) const
{
    if (!isMaster) return false;
    if (index >= MAX_SLAVES) return false;
    return slaves[index].active;
}

void EmcEspNow::update()
{
    if (!instance) return;

    if (isMaster)
    {
        // Send command to all active slaves
        for (int i = 0; i < MAX_SLAVES; i++)
        {
            if (slaves[i].active)
            {
                sendUnicast(slaves[i].mac, (uint8_t *)&masterCmdData, sizeof(master_cmd_t));
            }
        }
        // Check slave timeout
        checkSlaveTimeouts();
    }
    else
    {
        // Slave: send data to master if connected
        if (masterConnected)
        {
            // Send data if changed or periodically (heartbeat)
            bool dataChanged = memcmp(&slaveSendData, &lastSlaveSendData, sizeof(slave_data_t)) != 0;
            unsigned long now = millis();
            if (dataChanged || (now - lastHeartbeatSend) >= HEARTBEAT_INTERVAL_MS)
            {
                // Update sequence for change detection
                slaveSendData.sequence++;
                sendUnicast(masterMac, (uint8_t *)&slaveSendData, sizeof(slave_data_t));
                memcpy(&lastSlaveSendData, &slaveSendData, sizeof(slave_data_t));
                lastHeartbeatSend = now;
            }
        }
        else
        {
            // If master is not connected, send broadcast periodically to find the master
            if (millis() - broadcastMillis > 1000)
            {
                broadcastMillis = millis();
                sendBroadcast();
            }
        }

        // Check master timeout: if > 3 seconds no data from master, consider disconnected
        // if (masterConnected && (millis() - lastMasterRxTime) > PEER_TIMEOUT_MS)
        // {
        //     log_w("Master timeout, disconnecting");
        //     removePeer(masterMac);
        //     masterConnected = false;
        //     memset(masterMac, 0, 6);
        // }
    }
}

void EmcEspNow::onReceive(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (isMaster)
    {
        // Master receives data from slave
        if (len == sizeof(slave_data_t))
        {
            slave_data_t *rxData = (slave_data_t *)data;
            // Update data if sequence changes (or data is different)
            if (rxData->sequence != masterRecvData.sequence)
            {
                memcpy(&masterRecvData, rxData, sizeof(slave_data_t));
            }
            // Update or add slave
            updateSlave(recv_info->src_addr);
        }
        // Response to broadcast from slave (slave looking for master)
        else if (memcmp(recv_info->des_addr, BROADCAST_MAC_SLAVE, 6) == 0)
        {
            // Send broadcast response or directly add peer
            updateSlave(recv_info->src_addr);
            sendBroadcast(); // Notify slave that master is available
        }
    }
    else
    {
        // Slave receives data from master
        if (len == sizeof(master_cmd_t))
        {
            master_cmd_t *cmd = (master_cmd_t *)data;
            // Update command if sequence changes
            if (cmd->sequence != masterCmdData.sequence)
            {
                memcpy(&masterCmdData, cmd, sizeof(master_cmd_t));
            }
            // Update master connection status
            if (!masterConnected)
            {
                memcpy(masterMac, recv_info->src_addr, 6);
                addPeer(masterMac);
                masterConnected = true;
                log_d("Master connected: " MACSTR, MAC2STR(masterMac));
            }
            lastMasterRxTime = millis();
        }
        // Response to broadcast from master
        else if (memcmp(recv_info->des_addr, BROADCAST_MAC_MASTER, 6) == 0)
        {
            if (!masterConnected)
            {
                memcpy(masterMac, recv_info->src_addr, 6);
                addPeer(masterMac);
                masterConnected = true;
                log_d("Master found via broadcast: " MACSTR, MAC2STR(masterMac));
            }
            lastMasterRxTime = millis();
        }
    }
}

void EmcEspNow::onSend(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_FAIL)
    {
        log_d("Send failed, removing peer");
        if (instance)
            instance->removePeer(tx_info->des_addr);
    }
}

void EmcEspNow::espNowTask(void *pvParameters)
{
    while (true)
    {
        if (instance)
            instance->update();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

#endif