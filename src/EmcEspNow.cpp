#if defined(ESP32)

#include "EmcEspNow.h"

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
                             { instance->onReceive(recv_info, data, len); });

    this->isMaster = isMaster;
    instance = this;

    if (!isMaster)
    {
        addPeer(BROADCAST_MAC_SLAVE);
    }
    else
    {
        addPeer(BROADCAST_MAC_MASTER);
    }

    resetData();
    // xTaskCreatePinnedToCore(espNowTask, "espNowTask", 2048, NULL, 1, NULL, 0);
}

void EmcEspNow::addPeer(const uint8_t *peer_addr)
{
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(esp_now_peer_info_t));
    memcpy(peer.peer_addr, peer_addr, 6);
    peer.channel = ESPNOW_WIFI_CHANNEL;
    peer.encrypt = false;

    if (!esp_now_is_peer_exist(peer.peer_addr))
    {
        if (esp_now_add_peer(&peer) == ESP_OK)
        {
            log_d("Peer added: " MACSTR "\n", MAC2STR(peer.peer_addr));
            peers_t new_peer;
            new_peer.peerID = peers.size();
            memcpy(new_peer.peer_mac, peer.peer_addr, 6);
            peers.push_back(new_peer);
        }
        else
        {
            log_e("Failed to add peer");
        }
    }
}

void EmcEspNow::removePeer(const uint8_t *peer_mac)
{
    resetData();
    for (auto it = peers.begin(); it != peers.end(); ++it)
    {
        if (memcmp(it->peer_mac, peer_mac, 6) == 0)
        {
            esp_now_del_peer(peer_mac);
            peers.erase(it);
            log_d("Peer removed: " MACSTR "\n", MAC2STR(peer_mac));
            break;
        }
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

void EmcEspNow::sendBroadcast()
{
    if (!isMaster)
    {
        esp_now_send(BROADCAST_MAC_SLAVE, (uint8_t *)&BROADCAST_SLAVE_MESSAGE, strlen(BROADCAST_SLAVE_MESSAGE));
    }
    else
    {
        esp_now_send(BROADCAST_MAC_MASTER, (uint8_t *)&BROADCAST_MASTER_MESSAGE, strlen(BROADCAST_MASTER_MESSAGE));
    }
}

void EmcEspNow::sendUnicast(const uint8_t *peer_mac, const uint8_t *data, size_t len)
{
    esp_now_send(peer_mac, data, len);
    // esp_err_t result = esp_now_send(peer_mac, data, len);
    // if (result != ESP_OK) {
    //     removePeer(peer_mac); // Handle failure
    // }
}

void EmcEspNow::update()
{
    if (!isMaster && peers.size() == 1)
    {
        if (millis() - broadcastMillis > 100)
        {
            broadcastMillis = millis();
            sendBroadcast();
        }
        return;
    }

    if (isMaster)
    {
        // if (memcmp(&masterCmdData, &lastmasterCmdData, sizeof(master_cmd_t)) != 0)
        // {
        //     for (uint8_t i = 1; i < peers.size(); i++)
        //     {
        //         sendUnicast(peers[i].peer_mac, (uint8_t *)&masterCmdData, sizeof(master_cmd_t));
        //     }

        //     memcpy(&lastmasterCmdData, &masterCmdData, sizeof(master_cmd_t));
        // }
        for (uint8_t i = 1; i < peers.size(); i++)
        {
            sendUnicast(peers[i].peer_mac, (uint8_t *)&masterCmdData, sizeof(master_cmd_t));
        }
    }
    else
    {
        if (masterHasCommand)
            return;

        if (memcmp(&slaveSendData, &lastSlaveSendData, sizeof(slave_data_t)) != 0)
        {
            sendUnicast(peers[1].peer_mac, (uint8_t *)&slaveSendData, sizeof(slave_data_t)); // Master always at index 1
            memcpy(&lastSlaveSendData, &slaveSendData, sizeof(slave_data_t));
        }
    }
    // yield();
}

void EmcEspNow::onReceive(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (isMaster)
    {
        if (memcmp(recv_info->des_addr, BROADCAST_MAC_SLAVE, 6) == 0)
        {
            sendBroadcast();
            if (!esp_now_is_peer_exist(recv_info->src_addr))
            {
                addPeer(recv_info->src_addr);
            }
        }

        if (len == sizeof(slave_data_t))
        {
            if (memcmp(&masterRecvData, data, sizeof(slave_data_t)) != 0)
            {
                memcpy(&masterRecvData, data, sizeof(slave_data_t));
            }
        }
    }
    else
    {
        if (memcmp(recv_info->des_addr, BROADCAST_MAC_MASTER, 6) == 0)
        {
            if (!esp_now_is_peer_exist(recv_info->src_addr))
            {
                addPeer(recv_info->src_addr);
            }
        }

        if (len == sizeof(master_cmd_t))
        {
            masterHasCommand = true;
            if (memcmp(&masterCmdData, data, sizeof(master_cmd_t)) != 0)
            {
                memcpy(&masterCmdData, data, sizeof(master_cmd_t));
            }
            masterHasCommand = false;
        }
    }
}

void EmcEspNow::onSend(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_FAIL)
    {
        log_d("Failed to send unicast, removing peer...");
        instance->removePeer(mac_addr);
    }
}

void EmcEspNow::espNowTask(void *pvParameters)
{
    while (true)
    {
        instance->update();
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}

#endif