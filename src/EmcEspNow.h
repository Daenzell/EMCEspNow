/*
 * EmcEspNow.h
 *
 *  Created on: 22.09.2020
 *      Author: daenzell
 */

#if defined(ESP32)
#pragma once

/**
 * @file EmcEspNow.h
 * @brief ESP-NOW communication library
 *
 * This library provides an interface to communicate with other ESP32 boards
 * using the ESP-NOW protocol. It allows for peer-to-peer communication between
 * ESP32 boards and is designed to be used with the ESP32 slave boards to receive
 * data from the master board.
 *
 * The library provides a simple API to send and receive data between the master
 * and slave boards. The data is sent as a struct, which is defined in this
 * header file. The struct contains an array of bytes that can be used to send
 * any type of data.
 *
 * The library also provides a mechanism to register a callback function that
 * will be called when data is received from the master board. This callback
 * function can be used to process the received data in any way that is needed.
 */

#include <esp_now.h>
#include <WiFi.h>
#define ESPNOW_WIFI_CHANNEL 6

/**
 * @brief Define the WiFi channel used for ESP-NOW communication.
 *
 * The value of this define can be changed to use a different WiFi channel.
 * The default value is channel 6, which is the same as the default channel
 * used by the ESP32 WiFi library.
 */

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

/**
 * @class EmcEspNow
 * @brief Provides an interface for ESP-NOW communication between ESP32 devices.
 *
 * This class facilitates both broadcast and unicast communication using the ESP-NOW protocol.
 * It is designed to work in both master and slave roles, managing peers and handling data transmission.
 */
class EmcEspNow
{
    /**
     * @struct master_cmd_t
     * @brief Represents a command structure used by the master device.
     */
    typedef struct
    {
        uint8_t mainId = 0;   ///< Main command identifier
        uint8_t subId = 0;    ///< Sub-command identifier
        uint8_t index1 = 0;   ///< First index value
        uint8_t index2 = 0;   ///< Second index value
        float value = 0;      ///< Floating point value
        int32_t valueInt = 0; ///< Integer value
    } __attribute__((packed)) master_cmd_t;

    /**
     * @struct peers_t
     * @brief Represents a peer device in the ESP-NOW network.
     */
    typedef struct
    {
        uint8_t peerID;        ///< Unique identifier for the peer
        uint8_t peer_mac[6];   ///< MAC address of the peer
    } __attribute__((packed)) peers_t;

public:
    /**
     * @brief Initializes the ESP-NOW communication.
     * @param isMaster Indicates if the device is operating in master mode.
     */
    void begin(bool isMaster);

    /**
     * @brief Terminates the ESP-NOW communication and cleans up resources.
     */
    void end();

    /**
     * @brief Adds a peer to the ESP-NOW network.
     * @param peer_addr MAC address of the peer to be added.
     */
    void addPeer(const uint8_t *peer_addr);

    /**
     * @brief Removes a peer from the ESP-NOW network.
     * @param peer_mac MAC address of the peer to be removed.
     */
    void removePeer(const uint8_t *peer_mac);

    /**
     * @brief Sends a broadcast message to all peers.
     */
    void sendBroadcast();

    /**
     * @brief Sends a unicast message to a specific peer.
     * @param peer_mac MAC address of the target peer.
     * @param data Pointer to the data to be sent.
     * @param len Length of the data to be sent.
     */
    void sendUnicast(const uint8_t *peer_mac, const uint8_t *data, size_t len);

    /**
     * @brief Periodically updates the state of the ESP-NOW communication.
     */
    void update();

    /**
     * @brief Callback function for handling received data.
     * @param recv_info Information about the received data.
     * @param data Pointer to the received data.
     * @param len Length of the received data.
     */
    void onReceive(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

    /**
     * @brief Resets the internal data structures.
     */
    void resetData();

    slave_data_t slaveSendData;         ///< Data to be sent by the slave
    slave_data_t masterRecvData;        ///< Data received by the master

    master_cmd_t masterCmdData;         ///< Current command data from the master
    master_cmd_t lastmasterCmdData;     ///< Last command data from the master

    std::vector<peers_t> peers;         ///< List of peers in the network

private:
    slave_data_t lastSlaveSendData;     ///< Last data sent by the slave

    unsigned long broadcastMillis = 0;  ///< Timer for broadcast messages

    const char *BROADCAST_SLAVE_MESSAGE = "EMCFFBV2 Slave!";  ///< Broadcast message for slaves
    const char *BROADCAST_MASTER_MESSAGE = "EMCFFBV2 Master!"; ///< Broadcast message for master

    const uint8_t BROADCAST_MAC_SLAVE[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD}; ///< Broadcast MAC for slaves
    const uint8_t BROADCAST_MAC_MASTER[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}; ///< Broadcast MAC for master

    bool isMaster = false;              ///< Indicates if the device is in master mode
    bool masterHasCommand = false;      ///< Indicates if the master has a command to process

    static EmcEspNow *instance;         ///< Singleton instance of the class

    /**
     * @brief Callback function for handling send status.
     * @param mac_addr MAC address of the target peer.
     * @param status Status of the send operation.
     */
    static void onSend(const uint8_t *mac_addr, esp_now_send_status_t status);

    /**
     * @brief Task function for processing ESP-NOW events.
     * @param pvParameters Parameters for the task function.
     */
    static void espNowTask(void *pvParameters);
};

#endif