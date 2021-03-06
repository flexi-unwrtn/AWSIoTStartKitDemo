/*
 * Copyright 2010-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/**
 * @file shadow_sample.c
 * @brief A simple connected window example demonstrating the use of Thing Shadow
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

#include <signal.h>
#include <memory.h>
#include <sys/time.h>
#include <limits.h>


#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_shadow_interface.h"
#include "aws_iot_shadow_json_data.h"
#include "aws_iot_config.h"
#include "aws_iot_mqtt_interface.h"
#include "event_gpio.h"

enum{
	gpioUserKey1 = 15,
	gpioUserKey2 = 28,	
	gpioYellowLed = 84,
	gpioRedLed = 83,
	gpioGreenLed = 82,
	gpioOrangeLed = 76,
	gpioWhiteLed = 78,
	gpioBlueLed = 80,
	gpioPurpleLed = 81,
	gpioPinkLed = 79
};

unsigned int leds[] = {gpioYellowLed, gpioRedLed, gpioGreenLed, gpioOrangeLed, gpioWhiteLed, gpioBlueLed, gpioPurpleLed, gpioPinkLed}; 
char ledscolor[][10] = {"Yellow", "Red", "Green", "Orange", "White", "Blue"};
#define MAX_LENGTH_OF_UPDATE_JSON_BUFFER 200
int lastled = 0;
int lednum = 0;
jsonStruct_t ledSwitcher;
MQTTClient_t mqttClient;

/*!
 * The goal of this sample application is to demonstrate the capabilities of shadow.
 * This device(say Connected Window) will open the window of a room based on temperature
 * It can report to the Shadow the following parameters:
 *  1. temperature of the room (double)
 *  2. status of the window (open or close)
 * It can act on commands from the cloud. In this case it will open or close the window based on the json object "windowOpen" data[open/close]
 *
 * The two variables from a device's perspective are double temperature and bool windowOpen
 * The device needs to act on only on windowOpen variable, so we will create a primitiveJson_t object with callback
 The Json Document in the cloud will be
 {
 "reported": {
 "temperature": 0,
 "windowOpen": false
 },
 "desired": {
 "windowOpen": false
 }
 }
 */

#define ROOMTEMPERATURE_UPPERLIMIT 32.0f
#define ROOMTEMPERATURE_LOWERLIMIT 25.0f
#define STARTING_ROOMTEMPERATURE ROOMTEMPERATURE_LOWERLIMIT

static void simulateRoomTemperature(float *pRoomTemperature) {
	static float deltaChange;

	if (*pRoomTemperature >= ROOMTEMPERATURE_UPPERLIMIT) {
		deltaChange = -0.5f;
	} else if (*pRoomTemperature <= ROOMTEMPERATURE_LOWERLIMIT) {
		deltaChange = 0.5f;
	}

	*pRoomTemperature += deltaChange;
}

static bool checkLedSwitcher(char *color) {
	bool ret = false;
	static unsigned int lastValue1 = HIGH, lastValue2 = HIGH, i = 0;
	unsigned int value1, value2;

	gpio_export(gpioUserKey1);
	gpio_set_direction(gpioUserKey1, INPUT);
	gpio_get_value(gpioUserKey1, &value1);
	//INFO("UserKey1 value %d\n", value1);
	if((value1==LOW) && (lastValue1!=value1)) // one switch
	{
		i = (i+1)%lednum;
		strcpy(color, ledscolor[i]);
		ret = true;
	}
	lastValue1 = value1;
	
	gpio_export(gpioUserKey2);
	gpio_set_direction(gpioUserKey2, INPUT);
	gpio_get_value(gpioUserKey2, &value2);
	//INFO("UserKey2 value %d\n", value2);
	if((value2==LOW) && (lastValue2!=value2)) // one switch
	{
		i = (i+lednum-1)%lednum;
		strcpy(color, ledscolor[i]);
		ret = true;
	}
	lastValue2 = value2;		
	
	return ret;
}

void ShadowUpdateStatusCallback(const char *pThingName, ShadowActions_t action, Shadow_Ack_Status_t status,
		const char *pReceivedJsonDocument, void *pContextData) {

	if (status == SHADOW_ACK_TIMEOUT) {
		INFO("Update Timeout--");
	} else if (status == SHADOW_ACK_REJECTED) {
		INFO("Update RejectedXX");
	} else if (status == SHADOW_ACK_ACCEPTED) {
		INFO("Update Accepted !!");
	}
}

void windowActuate_Callback(const char *pJsonString, uint32_t JsonStringDataLen, jsonStruct_t *pContext) {
	if (pContext != NULL) {
		INFO("Delta - Window state changed to %d", *(bool *)(pContext->pData));
	}
}

void ledSwitcher_Callback(const char *pJsonString, uint32_t JsonStringDataLen, jsonStruct_t *pContext) {
	if (pContext != NULL) {
		char *color = (char *)(pContext->pData);
		INFO("Delta - Led Color changed to %s", color);

		unsigned int curled = leds[lastled];
		unsigned int nextled = -1, i = 0;

		for(i=0; i<lednum; i++)
		{
			if(strcmp(ledscolor[i], color) == 0)
			{
				nextled = leds[i];
				lastled = i;
			}
		}
				
		if(nextled != -1)
		{
			gpio_export(curled);
			gpio_set_direction(curled, OUTPUT);
			gpio_set_value(curled, HIGH);
			gpio_export(nextled);
			gpio_set_direction(nextled, OUTPUT);
			gpio_set_value(nextled, LOW);
			INFO("switch to led %d [GPIO %d]", lastled, nextled);

			IoT_Error_t rc = NONE_ERROR;
			char JsonDocumentBuffer[MAX_LENGTH_OF_UPDATE_JSON_BUFFER];
			size_t sizeOfJsonDocumentBuffer = sizeof(JsonDocumentBuffer) / sizeof(JsonDocumentBuffer[0]);
			char *pJsonStringToUpdate;
			rc = aws_iot_shadow_init_json_document(JsonDocumentBuffer, sizeOfJsonDocumentBuffer);
			if (rc == NONE_ERROR) {
				rc = aws_iot_shadow_add_reported(JsonDocumentBuffer, sizeOfJsonDocumentBuffer, 1, &ledSwitcher);
				if (rc == NONE_ERROR) {
					rc = aws_iot_finalize_json_document(JsonDocumentBuffer, sizeOfJsonDocumentBuffer);
					if (rc == NONE_ERROR) {
						INFO("Update Shadow Reported: %s", JsonDocumentBuffer);
						rc = aws_iot_shadow_update(&mqttClient, AWS_IOT_MY_THING_NAME, JsonDocumentBuffer, ShadowUpdateStatusCallback,
						NULL, 4, true);
					}
				}
			}
		}
		
	}
}

char certDirectory[PATH_MAX + 1] = "../../certs";
char HostAddress[255] = AWS_IOT_MQTT_HOST;
uint32_t port = AWS_IOT_MQTT_PORT;
uint8_t numPubs = 5;

void parseInputArgsForConnectParams(int argc, char** argv) {
	int opt;

	while (-1 != (opt = getopt(argc, argv, "h:p:c:n:"))) {
		switch (opt) {
		case 'h':
			strcpy(HostAddress, optarg);
			DEBUG("Host %s", optarg);
			break;
		case 'p':
			port = atoi(optarg);
			DEBUG("arg %s", optarg);
			break;
		case 'c':
			strcpy(certDirectory, optarg);
			DEBUG("cert root directory %s", optarg);
			break;
		case 'n':
			numPubs = atoi(optarg);
			DEBUG("num pubs %s", optarg);
			break;
		case '?':
			if (optopt == 'c') {
				ERROR("Option -%c requires an argument.", optopt);
			} else if (isprint(optopt)) {
				WARN("Unknown option `-%c'.", optopt);
			} else {
				WARN("Unknown option character `\\x%x'.", optopt);
			}
			break;
		default:
			ERROR("ERROR in command line argument parsing");
			break;
		}
	}

}

int main(int argc, char** argv) {
	IoT_Error_t rc = NONE_ERROR;
	int32_t i = 0, j = 0, k = 0;

	aws_iot_mqtt_init(&mqttClient);

	char JsonDocumentBuffer[MAX_LENGTH_OF_UPDATE_JSON_BUFFER];
	size_t sizeOfJsonDocumentBuffer = sizeof(JsonDocumentBuffer) / sizeof(JsonDocumentBuffer[0]);
	char *pJsonStringToUpdate;
	float temperature = 0.0;
	char ledColor[10] = "None";

	bool windowOpen = false;
	jsonStruct_t windowActuator;
	windowActuator.cb = windowActuate_Callback;
	windowActuator.pData = &windowOpen;
	windowActuator.pKey = "windowOpen";
	windowActuator.type = SHADOW_JSON_BOOL;

	jsonStruct_t temperatureHandler;
	temperatureHandler.cb = NULL;
	temperatureHandler.pKey = "temperature";
	temperatureHandler.pData = &temperature;
	temperatureHandler.type = SHADOW_JSON_FLOAT;

	ledSwitcher.cb = ledSwitcher_Callback;
	ledSwitcher.pData = ledColor;
	ledSwitcher.pKey = "ledColor";
	ledSwitcher.type = SHADOW_JSON_STRING;

	char rootCA[PATH_MAX + 1];
	char clientCRT[PATH_MAX + 1];
	char clientKey[PATH_MAX + 1];
	char CurrentWD[PATH_MAX + 1];
	char cafileName[] = AWS_IOT_ROOT_CA_FILENAME;
	char clientCRTName[] = AWS_IOT_CERTIFICATE_FILENAME;
	char clientKeyName[] = AWS_IOT_PRIVATE_KEY_FILENAME;

	parseInputArgsForConnectParams(argc, argv);

	lednum = ARRAY_SIZE(leds);
	for(k=0; k<lednum; k++)
	{
		gpio_export(leds[k]);
		gpio_set_direction(leds[k], OUTPUT);
		gpio_set_value(leds[k], HIGH);
	}
	gpio_export(leds[0]);
	gpio_set_direction(leds[0], OUTPUT);
	gpio_set_value(leds[0], LOW);

	INFO("\nAWS IoT SDK Version(dev) %d.%d.%d-%s\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

	getcwd(CurrentWD, sizeof(CurrentWD));
	sprintf(rootCA, "%s/%s/%s", CurrentWD, certDirectory, cafileName);
	sprintf(clientCRT, "%s/%s/%s", CurrentWD, certDirectory, clientCRTName);
	sprintf(clientKey, "%s/%s/%s", CurrentWD, certDirectory, clientKeyName);

	DEBUG("Using rootCA %s", rootCA);
	DEBUG("Using clientCRT %s", clientCRT);
	DEBUG("Using clientKey %s", clientKey);

	ShadowParameters_t sp = ShadowParametersDefault;
	sp.pMyThingName = AWS_IOT_MY_THING_NAME;
	sp.pMqttClientId = AWS_IOT_MQTT_CLIENT_ID;
	sp.pHost = HostAddress;
	sp.port = port;
	sp.pClientCRT = clientCRT;
	sp.pClientKey = clientKey;
	sp.pRootCA = rootCA;

	INFO("Shadow Init");
	rc = aws_iot_shadow_init(&mqttClient);

	INFO("Shadow Connect");
	rc = aws_iot_shadow_connect(&mqttClient, &sp);

	if (NONE_ERROR != rc) {
		ERROR("Shadow Connection Error %d", rc);
	}

	//rc = aws_iot_shadow_register_delta(&mqttClient, &windowActuator);
	rc = aws_iot_shadow_register_delta(&mqttClient, &ledSwitcher);

	if (NONE_ERROR != rc) {
		ERROR("Shadow Register Delta Error");
	}
	temperature = STARTING_ROOMTEMPERATURE;

	// loop and publish a change in temperature
	while (NONE_ERROR == rc) {
		rc = aws_iot_shadow_yield(&mqttClient, 200);
		//INFO("\n=======================================================================================\n");
		//INFO("On Device: window state %s", windowOpen?"true":"false");
		//simulateRoomTemperature(&temperature);
		//INFO("On Device: cur led color %s", (char *)(ledSwitcher.pData));
		sleep(1);
		if(checkLedSwitcher(ledColor) == false)
			continue;

		INFO("On Device: want led color %s", (char *)(ledSwitcher.pData));
		rc = aws_iot_shadow_init_json_document(JsonDocumentBuffer, sizeOfJsonDocumentBuffer);
		if (rc == NONE_ERROR) {
			//rc = aws_iot_shadow_add_reported(JsonDocumentBuffer, sizeOfJsonDocumentBuffer, 2, &temperatureHandler, &windowActuator);
			rc = aws_iot_shadow_add_desired(JsonDocumentBuffer, sizeOfJsonDocumentBuffer, 1, &ledSwitcher);
			if (rc == NONE_ERROR) {
				rc = aws_iot_finalize_json_document(JsonDocumentBuffer, sizeOfJsonDocumentBuffer);
				if (rc == NONE_ERROR) {
					INFO("Update Shadow Desired: %s", JsonDocumentBuffer);
					rc = aws_iot_shadow_update(&mqttClient, AWS_IOT_MY_THING_NAME, JsonDocumentBuffer, ShadowUpdateStatusCallback,
					NULL, 4, true);
				}
			}
		}
		//INFO("*****************************************************************************************\n");
	}

	if (NONE_ERROR != rc) {
		ERROR("An error occurred in the loop %d", rc);
	}

	INFO("Disconnecting");
	rc = aws_iot_shadow_disconnect(&mqttClient);

	if (NONE_ERROR != rc) {
		ERROR("Disconnect error %d", rc);
	}

	return rc;
}
