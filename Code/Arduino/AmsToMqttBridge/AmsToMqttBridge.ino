/*
 Name:		AmsToMqttBridge.ino
 Created:	3/13/2018 7:40:28 PM
 Author:	roarf
*/

#include <ESP8266WiFi.h>
#include <RemoteDebug.h> // Remote debug over telnet - not recommended for production, only for development     
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <DoubleResetDetector.h>  // https://github.com/datacute/DoubleResetDetector
#include <HanReader.h>
#include <Kaifa.h>
#include <Kamstrup.h>
#include "configuration.h"
#include "accesspoint.h"

#define WIFI_CONNECTION_TIMEOUT 30000;
#define LED_PIN LED_BUILTIN // The blue on-board LED of the ESP

// Object used to boot as Access Point
accesspoint ap;

// WiFi client and MQTT client
WiFiClient *client;
PubSubClient mqtt;

// Objects used for debugging
HardwareSerial* debugger = NULL;
RemoteDebug Debug;

/***********************************************
*  Double Reset detector
***********************************************/
// Number of seconds after reset during which a 
// subseqent reset will be considered a double reset.
#define DRD_TIMEOUT 10

// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0

DoubleResetDetector drd(DRD_TIMEOUT, DRD_ADDRESS);

// The HAN Port reader, used to read serial data and decode DLMS
HanReader hanReader;

// the setup function runs once when you press reset or power the board
void setup() {
	// Uncomment to debug over the same port as used for HAN communication
	debugger = &Serial;

  Debug.begin("AMS2MQTT", Debug.INFO); 
	
	if (debugger) {
		// Setup serial port for debugging
		debugger->begin(2400, SERIAL_8E1);
		while (!&debugger);
		debugger->println("Started...");
    rdebugI("Started...");
	}

	pinMode(LED_PIN, OUTPUT);
	
	// Initialize the AP
	ap.setup(Serial, drd.detectDoubleReset(), LED_PIN);

	if (!ap.isActivated)
	{
		setupWiFi();
		hanReader.setup(&Serial, 2400, SERIAL_8E1, debugger);
		
		// Compensate for the known Kaifa bug
		hanReader.compensateFor09HeaderBug = (ap.config.meterType == 1);
	}
}

// the loop function runs over and over again until power down or reset
void loop()
{
	// Only do normal stuff if we're not booted as AP
	if (!ap.loop())
	{
		// turn off the blue LED
		digitalWrite(LED_PIN, HIGH);

		// allow the MQTT client some resources
		mqtt.loop();
		delay(10); // <- fixes some issues with WiFi stability

		// Reconnect to WiFi and MQTT as needed
		if (!mqtt.connected()) {
			MQTT_connect();
		}
		else
		{
			// Read data from the HAN port
			readHanPort();
		}
    //    Double reset detection enabled
    drd.loop();
	}
	else
	{
		// Continously flash the blue LED when AP mode
		if (millis() / 1000 % 2 == 0)
			digitalWrite(LED_PIN, LOW);
		else
			digitalWrite(LED_PIN, HIGH);
	}
  Debug.handle();
}

void setupWiFi()
{
	// Turn off AP
	WiFi.enableAP(false);
	
	// Connect to WiFi
  WiFi.hostname("AMS2MQTT");
	WiFi.begin(ap.config.ssid, ap.config.ssidPassword);
	
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
	}
	
	// Initialize WiFi and MQTT clients
	if (ap.config.isSecure())
		client = new WiFiClientSecure();
	else
		client = new WiFiClient();
	mqtt = PubSubClient(*client);
	mqtt.setServer(ap.config.mqtt, ap.config.mqttPort);

 if (MQTT_MAX_PACKET_SIZE < 500) {
  rdebugW("PubSubClient.h MQTT_MAX_PACKET_SIZE should be set to 1024, is %d!", MQTT_MAX_PACKET_SIZE);
 }

	// Direct incoming MQTT messages
	if (ap.config.mqttSubscribeTopic != 0 && strlen(ap.config.mqttSubscribeTopic) > 0)
		mqtt.setCallback(mqttMessageReceived);

	// Connect to the MQTT server
	MQTT_connect();

	// Notify everyone we're here!
	sendMqttData("Connected!");
  rdebugI("Connected!");
}

void mqttMessageReceived(char* topic, unsigned char* payload, unsigned int length)
{
	// make the incoming message a null-terminated string
	char message[1000];
	for (int i = 0; i < length; i++)
		message[i] = payload[i];
	message[length] = 0;

	if (debugger) {
		debugger->println("Incoming MQTT message:");
		debugger->print("[");
		debugger->print(topic);
		debugger->print("] ");
		debugger->println(message);
	}
  rdebugI("Incoming MQTT message: [%s] \n", topic);
  rdebugI("%s\n", message);

	// Do whatever needed here...
	// Ideas could be to query for values or to initiate OTA firmware update
}

void readHanPort()
{
	if (hanReader.read())
	{
		// Flash LED on, this shows us that data is received
		digitalWrite(LED_PIN, LOW);

		// Get the list identifier
		int listSize = hanReader.getListSize();
    rdebugI("Listsize: %d\n", listSize);

		switch (ap.config.meterType)
		{
		case 1: // Kaifa
			readHanPort_Kaifa(listSize);
			break;
		case 2: // Aidon
			readHanPort_Aidon(listSize);
			break;
		case 3: // Kamstrup
			readHanPort_Kamstrup(listSize);
			break;
		default:
			debugger->print("Meter type ");
			debugger->print(ap.config.meterType, HEX);
			debugger->println(" is unknown");

      rdebugW("Meter type %d is unknown\n", ap.config.meterType);
			delay(10000);
			break;
		}

		// Flash LED off
		digitalWrite(LED_PIN, HIGH);
	}
}

void readHanPort_Aidon(int listSize)
{
	if (debugger)
		debugger->println("Meter type Aidon is not yet implemented");
  rdebugE("Meter type Aidon is not yet implemented\n");
	delay(10000);
}

void readHanPort_Kamstrup(int listSize)
{
	// Only care for the ACtive Power Imported, which is found in the first list
	if (listSize == (int)Kamstrup::List1 || listSize == (int)Kamstrup::List2)
	{
		if (listSize == (int)Kamstrup::List1)
		{
			String id = hanReader.getString((int)Kamstrup_List1::ListVersionIdentifier);
			if (debugger) debugger->println(id);
		}
		else if (listSize == (int)Kamstrup::List2)
		{
			String id = hanReader.getString((int)Kamstrup_List2::ListVersionIdentifier);
			if (debugger) debugger->println(id);
		}

		// Get the timestamp (as unix time) from the package
		time_t time = hanReader.getPackageTime();
		if (debugger) debugger->print("Time of the package is: ");
		if (debugger) debugger->println(time);

    rdebugI("Time of the package is: %s\n", time);

		// Define a json object to keep the data
		StaticJsonBuffer<500> jsonBuffer;
		JsonObject& root = jsonBuffer.createObject();

		// Any generic useful info here
		root["id"] = WiFi.macAddress();
		root["up"] = millis();
		root["t"] = time;

		// Add a sub-structure to the json object, 
		// to keep the data from the meter itself
		JsonObject& data = root.createNestedObject("data");

		// Based on the list number, get all details 
		// according to OBIS specifications for the meter
		if (listSize == (int)Kamstrup::List1)
		{
			data["lv"] = hanReader.getString((int)Kamstrup_List1::ListVersionIdentifier);
			data["id"] = hanReader.getString((int)Kamstrup_List1::MeterID);
			data["type"] = hanReader.getString((int)Kamstrup_List1::MeterType);
			data["P"] = hanReader.getInt((int)Kamstrup_List1::ActiveImportPower);
			data["Q"] = hanReader.getInt((int)Kamstrup_List1::ReactiveImportPower);
			data["I1"] = hanReader.getInt((int)Kamstrup_List1::CurrentL1);
			data["I2"] = hanReader.getInt((int)Kamstrup_List1::CurrentL2);
			data["I3"] = hanReader.getInt((int)Kamstrup_List1::CurrentL3);
			data["U1"] = hanReader.getInt((int)Kamstrup_List1::VoltageL1);
			data["U2"] = hanReader.getInt((int)Kamstrup_List1::VoltageL2);
			data["U3"] = hanReader.getInt((int)Kamstrup_List1::VoltageL3);
		}
		else if (listSize == (int)Kamstrup::List2)
		{
			data["lv"] = hanReader.getString((int)Kamstrup_List2::ListVersionIdentifier);;
			data["id"] = hanReader.getString((int)Kamstrup_List2::MeterID);
			data["type"] = hanReader.getString((int)Kamstrup_List2::MeterType);
			data["P"] = hanReader.getInt((int)Kamstrup_List2::ActiveImportPower);
			data["Q"] = hanReader.getInt((int)Kamstrup_List2::ReactiveImportPower);
			data["I1"] = hanReader.getInt((int)Kamstrup_List2::CurrentL1);
			data["I2"] = hanReader.getInt((int)Kamstrup_List2::CurrentL2);
			data["I3"] = hanReader.getInt((int)Kamstrup_List2::CurrentL3);
			data["U1"] = hanReader.getInt((int)Kamstrup_List2::VoltageL1);
			data["U2"] = hanReader.getInt((int)Kamstrup_List2::VoltageL2);
			data["U3"] = hanReader.getInt((int)Kamstrup_List2::VoltageL3);
			data["tPI"] = hanReader.getInt((int)Kamstrup_List2::CumulativeActiveImportEnergy);
			data["tPO"] = hanReader.getInt((int)Kamstrup_List2::CumulativeActiveExportEnergy);
			data["tQI"] = hanReader.getInt((int)Kamstrup_List2::CumulativeReactiveImportEnergy);
			data["tQO"] = hanReader.getInt((int)Kamstrup_List2::CumulativeReactiveExportEnergy);
		}

		// Write the json to the debug port
		if (debugger) {
			debugger->print("Sending data to MQTT: ");
			root.printTo(*debugger);
			debugger->println();
		}
    rdebugI("Sending data to MQTT\n");

		// Make sure we have configured a publish topic
		if (ap.config.mqttPublishTopic == 0 || strlen(ap.config.mqttPublishTopic) == 0)
			return;

		// Publish the json to the MQTT server
		char msg[1024];
		root.printTo(msg, 1024);
		mqtt.publish(ap.config.mqttPublishTopic, msg);
	}
}


void readHanPort_Kaifa(int listSize) 
{
	// Only care for the ACtive Power Imported, which is found in the first list
	if (listSize == (int)Kaifa::List1 || listSize == (int)Kaifa::List2 || listSize == (int)Kaifa::List3)
	{
		if (listSize == (int)Kaifa::List1)
		{
			if (debugger) debugger->println(" (list #1 has no ID)");
      rdebugI("(list #1 has no ID)\n");
		}
		else
		{
			String id = hanReader.getString((int)Kaifa_List2::ListVersionIdentifier);
			if (debugger) debugger->println(id);
		}

		// Get the timestamp (as unix time) from the package
		time_t time = hanReader.getPackageTime();
		if (debugger) debugger->print("Time of the package is: ");
		if (debugger) debugger->println(time);

    rdebugI("Time of the package is: %s\n", time);
    
		// Define a json object to keep the data
		//StaticJsonBuffer<500> jsonBuffer;
		DynamicJsonBuffer jsonBuffer;
		JsonObject& root = jsonBuffer.createObject();

		// Any generic useful info here
		root["id"] = WiFi.macAddress();
		root["up"] = millis();
		root["t"] = time;

		// Add a sub-structure to the json object, 
		// to keep the data from the meter itself
		JsonObject& data = root.createNestedObject("data");

    data["lSize"] = listSize;

		// Based on the list number, get all details 
		// according to OBIS specifications for the meter
		if (listSize == (int)Kaifa::List1)
		{
			data["P"] = hanReader.getInt((int)Kaifa_List1::ActivePowerImported);
		}
		else if (listSize == (int)Kaifa::List2)
		{
			data["lv"] = hanReader.getString((int)Kaifa_List2::ListVersionIdentifier);
			data["id"] = hanReader.getString((int)Kaifa_List2::MeterID);
			data["type"] = hanReader.getString((int)Kaifa_List2::MeterType);
			data["P"] = hanReader.getInt((int)Kaifa_List2::ActiveImportPower);
			data["Q"] = hanReader.getInt((int)Kaifa_List2::ReactiveImportPower);
			data["I1"] = hanReader.getInt((int)Kaifa_List2::CurrentL1);
			data["I2"] = hanReader.getInt((int)Kaifa_List2::CurrentL2);
			data["I3"] = hanReader.getInt((int)Kaifa_List2::CurrentL3);
			data["U1"] = hanReader.getInt((int)Kaifa_List2::VoltageL1)/10;
			data["U2"] = hanReader.getInt((int)Kaifa_List2::VoltageL2)/10;
			data["U3"] = hanReader.getInt((int)Kaifa_List2::VoltageL3)/10;
		}
		else if (listSize == (int)Kaifa::List3)
		{
			data["lv"] = hanReader.getString((int)Kaifa_List3::ListVersionIdentifier);;
			data["id"] = hanReader.getString((int)Kaifa_List3::MeterID);
			data["type"] = hanReader.getString((int)Kaifa_List3::MeterType);
			data["P"] = hanReader.getInt((int)Kaifa_List3::ActiveImportPower);
			data["Q"] = hanReader.getInt((int)Kaifa_List3::ReactiveImportPower);
			data["I1"] = hanReader.getInt((int)Kaifa_List3::CurrentL1);
			data["I2"] = hanReader.getInt((int)Kaifa_List3::CurrentL2);
			data["I3"] = hanReader.getInt((int)Kaifa_List3::CurrentL3);
			data["U1"] = hanReader.getInt((int)Kaifa_List3::VoltageL1)/10;
			data["U2"] = hanReader.getInt((int)Kaifa_List3::VoltageL2)/10;
			data["U3"] = hanReader.getInt((int)Kaifa_List3::VoltageL3)/10;
			data["tPI"] = hanReader.getInt((int)Kaifa_List3::CumulativeActiveImportEnergy);
			data["tPO"] = hanReader.getInt((int)Kaifa_List3::CumulativeActiveExportEnergy);
			data["tQI"] = hanReader.getInt((int)Kaifa_List3::CumulativeReactiveImportEnergy);
			data["tQO"] = hanReader.getInt((int)Kaifa_List3::CumulativeReactiveExportEnergy);
		}

		// Write the json to the debug port
		if (debugger) {
			debugger->print("Sending data to MQTT: ");
			root.printTo(*debugger);
			debugger->println();
		}

    rdebugI("Sending data to MQTT\n");

		// Make sure we have configured a publish topic
		if (ap.config.mqttPublishTopic == 0 || strlen(ap.config.mqttPublishTopic) == 0)
			return;

		// Publish the json to the MQTT server
		char msg[1024];
		root.printTo(msg, 1024);
		mqtt.publish(ap.config.mqttPublishTopic, msg);
	}
}

// Function to connect and reconnect as necessary to the MQTT server.
// Should be called in the loop function and it will take care if connecting.
void MQTT_connect() 
{
	// Connect to WiFi access point.
	if (debugger)
	{
		debugger->println(); 
		debugger->println();
		debugger->print("Connecting to WiFi network ");
		debugger->println(ap.config.ssid);
	}

	if (WiFi.status() != WL_CONNECTED)
	{
		// Make one first attempt at connect, this seems to considerably speed up the first connection
		WiFi.disconnect();
		WiFi.begin(ap.config.ssid, ap.config.ssidPassword);
		delay(1000);
	}

	// Wait for the WiFi connection to complete
	long vTimeout = millis() + WIFI_CONNECTION_TIMEOUT;
	while (WiFi.status() != WL_CONNECTED) {
		delay(50);
		if (debugger) debugger->print(".");
		
		// If we timed out, disconnect and try again
		if (vTimeout < millis())
		{
			if (debugger)
			{
				debugger->print("Timout during connect. WiFi status is: ");
				debugger->println(WiFi.status());
			}
			WiFi.disconnect();
			WiFi.begin(ap.config.ssid, ap.config.ssidPassword);
			vTimeout = millis() + WIFI_CONNECTION_TIMEOUT;
		}
		yield();
	}

	if (debugger) {
		debugger->println();
		debugger->println("WiFi connected");
		debugger->println("IP address: ");
		debugger->println(WiFi.localIP());
		debugger->print("\nconnecting to MQTT: ");
		debugger->print(ap.config.mqtt);
		debugger->print(", port: ");
		debugger->print(ap.config.mqttPort);
		debugger->println();
	}

  rdebugI("\n\nWifi connected\n");
  rdebugI("Connecting to MQTT: %s\n", ap.config.mqtt);
  rdebugI(", port: %d\n", ap.config.mqttPort);

	// Wait for the MQTT connection to complete
	while (!mqtt.connected()) {
		
		// Connect to a unsecure or secure MQTT server
		if ((ap.config.mqttUser == 0 && mqtt.connect(ap.config.mqttClientID)) || 
			(ap.config.mqttUser != 0 && mqtt.connect(ap.config.mqttClientID, ap.config.mqttUser, ap.config.mqttPass)))
		{
			if (debugger) debugger->println("\nSuccessfully connected to MQTT!");
     rdebugI("\nSuccessfully connected to MQTT!");

			// Subscribe to the chosen MQTT topic, if set in configuration
			if (ap.config.mqttSubscribeTopic != 0 && strlen(ap.config.mqttSubscribeTopic) > 0)
			{
				mqtt.subscribe(ap.config.mqttSubscribeTopic);
				if (debugger) debugger->printf("  Subscribing to [%s]\r\n", ap.config.mqttSubscribeTopic);
        rdebugI("  Subscribing to [%s]\r\n", ap.config.mqttSubscribeTopic);
			}
		}
		else
		{
			if (debugger)
			{
				debugger->print(".");
				debugger->print("failed, mqtt.state() = ");
				debugger->print(mqtt.state());
				debugger->println(" trying again in 5 seconds");
			}

     rdebugE(".failed, mqtt.state() = %s trying again in 5 seconds\n", mqtt.state());

			// Wait 2 seconds before retrying
			mqtt.disconnect();
			delay(2000);
		}

		// Allow some resources for the WiFi connection
		yield();
	}
}

// Send a simple string embedded in json over MQTT
void sendMqttData(String data)
{
	// Make sure we have configured a publish topic
	if (ap.config.mqttPublishTopic == 0 || strlen(ap.config.mqttPublishTopic) == 0)
		return;

	// Make sure we're connected
	if (!client->connected() || !mqtt.connected()) {
		MQTT_connect();
	}

	// Build a json with the message in a "data" attribute
	DynamicJsonBuffer jsonBuffer;
	JsonObject& json = jsonBuffer.createObject();
	json["id"] = WiFi.macAddress();
	json["up"] = millis();
	json["data"] = data;

	// Stringify the json
	String msg;
	json.printTo(msg);

	// Send the json over MQTT
	mqtt.publish(ap.config.mqttPublishTopic, msg.c_str());
}
