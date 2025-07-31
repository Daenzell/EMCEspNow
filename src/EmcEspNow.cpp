/*
 * EmcEspNow.cpp
 *
 *  Created on: 22.09.2020
 *      Author: daenzell
 */

#if defined(ESP32)

#include "EmcEspNow.h"

EmcEspNow *EmcEspNow::instance = nullptr;

/**
 * @brief Initialize the ESP-NOW communication module in either master or slave mode.
 *
 * This method must be called before any other methods of the EmcEspNow class.
 * It initializes the ESP-NOW module and registers the callbacks for sending
 * and receiving messages.
 *
 * @param isMaster If true, the ESP-NOW module will operate in master mode.
 */
void EmcEspNow::begin(bool isMaster)
{
    // Set WiFi mode to station and set the channel
    WiFi.mode(WIFI_STA);
    WiFi.setChannel(ESPNOW_WIFI_CHANNEL);

    if (esp_now_init() != ESP_OK)
    {
        // Error handling
        log_e("Failed to initialize ESP-NOW");
        return;
    }

    // Register callbacks for send and receive events
    esp_now_register_send_cb(onSend);
    esp_now_register_recv_cb([](const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
                             { instance->onReceive(recv_info, data, len); });

    // Set the mode
    this->isMaster = isMaster;

    // Set the instance
    instance = this;

    // Add the broadcast peer
    if (!isMaster)
    {
        addPeer(BROADCAST_MAC_SLAVE);
    }
    else
    {
        addPeer(BROADCAST_MAC_MASTER);
    }

    // Reset the data
    resetData();

    // Start the task
    // xTaskCreatePinnedToCore(espNowTask, "espNowTask", 2048, NULL, 1, NULL, 0);
}

/**
 * @brief Ends the ESP-NOW communication and cleans up resources.
 *
 * This function is responsible for properly shutting down the ESP-NOW 
 * communication by unregistering callbacks, removing peers, deinitializing 
 * ESP-NOW, and turning off WiFi. It also resets the instance pointer.
 */
void EmcEspNow::end()
{
    resetData(); // Clear any stored data

    // Unregister callbacks to prevent any during shutdown
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();
    
    // Remove all peers from the ESP-NOW network
    for (auto it = peers.begin(); it != peers.end(); ++it)
    {
        esp_now_del_peer(it->peer_mac);
    }

    // Deinitialize the ESP-NOW communication protocol
    esp_now_deinit();

    // Turn off the WiFi to save power
    WiFi.mode(WIFI_OFF);

    // Clear the instance pointer and peer list
    instance = nullptr;
    peers.clear();
}

/**
 * @brief Adds a new peer to the ESP-NOW network.
 *
 * This function adds a new peer device to the ESP-NOW network by creating a
 * peer info struct and using the esp_now_add_peer() function to add the peer.
 * If the peer does not already exist, it is added to the list of peers.
 * @param[in] peer_addr The MAC address of the peer device to add.
 */
void EmcEspNow::addPeer(const uint8_t *peer_addr)
{
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(esp_now_peer_info_t));
    memcpy(peer.peer_addr, peer_addr, 6);
    peer.channel = ESPNOW_WIFI_CHANNEL;
    peer.encrypt = false;

    // Check if the peer already exists in the list of peers
    if (!esp_now_is_peer_exist(peer.peer_addr))
    {
        // Add the peer to the list of peers if it does not already exist
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

/**
 * @brief Removes a peer from the ESP-NOW network.
 *
 * This function removes a peer device from the ESP-NOW network by deleting the
 * peer from the list of peers and calling the esp_now_del_peer() function to
 * remove the peer. After removing the peer, the data is reset to prevent any
 * further communication with the peer.
 * @param[in] peer_mac The MAC address of the peer device to remove.
 */
void EmcEspNow::removePeer(const uint8_t *peer_mac)
{
    // Reset the data to prevent any further communication with the peer
    resetData();

    // Iterate over the list of peers and find the peer to remove
    for (auto it = peers.begin(); it != peers.end(); ++it)
    {
        if (memcmp(it->peer_mac, peer_mac, 6) == 0)
        {
            // Remove the peer from the list of peers
            esp_now_del_peer(peer_mac);
            peers.erase(it);
            log_d("Peer removed: " MACSTR "\n", MAC2STR(peer_mac));
            break;
        }
    }
}

/**
 * @brief Resets the data used for communication to its initial state.
 *
 * This function is used to reset the data used for communication with the
 * master device to its initial state. This is necessary to prevent any
 * further communication with the master device after the connection has been
 * closed.
 */
void EmcEspNow::resetData()
{
    // Reset the data used for communication
    memset(&slaveSendData, 0, sizeof(slave_data_t));
    memset(&lastSlaveSendData, 0, sizeof(slave_data_t));
    memset(&masterRecvData, 0, sizeof(slave_data_t));
    memset(&masterCmdData, 0, sizeof(master_cmd_t));
    memset(&lastmasterCmdData, 0, sizeof(master_cmd_t));
}

/**
 * @brief Sends a broadcast message to all peers in the ESP-NOW network.
 *
 * This function is used to send a broadcast message to all peers in the ESP-NOW
 * network. The message is sent using the esp_now_send() function and the
 * broadcast MAC address of the slave or master device is used as the destination
 * address depending on the mode of the device.
 */
void EmcEspNow::sendBroadcast()
{
    // Send the broadcast message using the broadcast MAC address of the slave or master device
    if (!isMaster)
    {
        // Send the broadcast message to all slave devices
        esp_now_send(BROADCAST_MAC_SLAVE, (uint8_t *)&BROADCAST_SLAVE_MESSAGE, strlen(BROADCAST_SLAVE_MESSAGE));
    }
    else
    {
        // Send the broadcast message to all master devices
        esp_now_send(BROADCAST_MAC_MASTER, (uint8_t *)&BROADCAST_MASTER_MESSAGE, strlen(BROADCAST_MASTER_MESSAGE));
    }
}

/**
 * @brief Sends a unicast message to a specific peer in the ESP-NOW network.
 *
 * This function is used to send a unicast message to a specific peer in the ESP-NOW
 * network. The message is sent using the esp_now_send() function and the
 * destination address of the peer is specified.
 *
 * @param[in] peer_mac The MAC address of the peer device to send the message to.
 * @param[in] data The data to be sent to the peer device.
 * @param[in] len The length of the data to be sent.
 */
void EmcEspNow::sendUnicast(const uint8_t *peer_mac, const uint8_t *data, size_t len)
{
    esp_now_send(peer_mac, data, len);
    // Handle failure
    // esp_err_t result = esp_now_send(peer_mac, data, len);
    // if (result != ESP_OK) {
    //     removePeer(peer_mac);
    // }
}

/**
 * @brief Periodically sends the latest data to all peers in the ESP-NOW network.
 *
 * This function is called periodically to send the latest data to all peers in the
 * ESP-NOW network. If the device is in master mode, it sends the latest command data
 * to all slave devices. If the device is in slave mode, it sends the latest data to
 * the master device.
 */
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
        // If the master command data has changed, send it to all slave devices
        // if (memcmp(&masterCmdData, &lastmasterCmdData, sizeof(master_cmd_t)) != 0)
        // {
        //     for (uint8_t i = 1; i < peers.size(); i++)
        //     {
        //         sendUnicast(peers[i].peer_mac, (uint8_t *)&masterCmdData, sizeof(master_cmd_t));
        //     }

        //     memcpy(&lastmasterCmdData, &masterCmdData, sizeof(master_cmd_t));
        // }
        // Send the master command data to all slave devices
        for (uint8_t i = 1; i < peers.size(); i++)
        {
            sendUnicast(peers[i].peer_mac, (uint8_t *)&masterCmdData, sizeof(master_cmd_t));
        }
    }
    else
    {
        if (masterHasCommand)
            return;

        // If the slave data has changed, send it to the master device
        if (memcmp(&slaveSendData, &lastSlaveSendData, sizeof(slave_data_t)) != 0)
        {
            sendUnicast(peers[1].peer_mac, (uint8_t *)&slaveSendData, sizeof(slave_data_t)); // Master always at index 1
            memcpy(&lastSlaveSendData, &slaveSendData, sizeof(slave_data_t));
        }
    }
    // yield();
}

/**
 * @brief Receive callback function for ESP-NOW messages.
 *
 * This function is called when a message is received from another ESP-NOW device.
 * The message is processed according to the mode of operation of the device.
 * If the device is in master mode, it processes the message as a slave device.
 * If the device is in slave mode, it processes the message as a master device.
 *
 * @param[in] recv_info The information about the received message.
 * @param[in] data The data received in the message.
 * @param[in] len The length of the data received in the message.
 */
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

/**
 * @brief Called when a message is sent using the esp_now_send() function.
 *
 * This function is called when a message is sent using the esp_now_send() function.
 * If the message fails to send, it removes the peer from the list of peers.
 *
 * @param[in] mac_addr The MAC address of the device that sent the message.
 * @param[in] status The status of the message send operation.
 */
void EmcEspNow::onSend(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_FAIL)
    {
        log_d("Failed to send unicast, removing peer...");
        instance->removePeer(mac_addr);
    }
}

/**
 * @brief Periodic task to update the ESP-NOW communication.
 *
 * This task is responsible for periodically sending the latest data to all
 * peers in the ESP-NOW network. If the device is in master mode, it sends the
 * latest command data to all slave devices. If the device is in slave mode, it
 * sends the latest data to the master device.
 */
void EmcEspNow::espNowTask(void *pvParameters)
{
    while (true)
    {
        instance->update();
        vTaskDelay(1 / portTICK_PERIOD_MS); // 1 ms delay between updates
    }
}

#endif