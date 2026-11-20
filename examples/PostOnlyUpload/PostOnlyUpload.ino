/*
 * Copyright (c) 2026 Marc D. Spencer.
 * SPDX-License-Identifier: MPL-2.0
 */

#include <LittleFS.h>
#include <MDS_HTTP_FS.h>

#if defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

constexpr char WifiSsid[] = "YOUR_SSID";
constexpr char WifiPassword[] = "YOUR_PASSWORD";
constexpr char UploadRoute[] = "/upload-file";
constexpr char UploadRoot[] = "/uploads";

// Enable once to initialize a blank LittleFS partition.
constexpr bool FormatOnMountFailure = false;

MDS_HTTP http;

// Mount LittleFS with the same format policy on both Arduino cores.
bool mountLittleFS() {
#if defined(ARDUINO_ARCH_ESP8266)
	return LittleFS.setConfig(LittleFSConfig(FormatOnMountFailure)) && LittleFS.begin();
#else
	return LittleFS.begin(FormatOnMountFailure);
#endif
}

// Register the file upload route before starting the network listener.
void setup() {
	Serial.begin(115200);

	if (!mountLittleFS()) {
		Serial.println("Could not mount LittleFS.");
		return;
	}

	MDS_HTTP_FS::install(http, UploadRoute, LittleFS, UploadRoot);

	http.setNetworkReadyCallback([]() {
		// Let the listener follow the station connection.
		return WiFi.status() == WL_CONNECTED;
	});

	http.setStateChangeCallback([](MDS_HTTP::State, MDS_HTTP::State current) {
		// Print the upload address as soon as the listener starts.
		if (current == MDS_HTTP::State::Listening) {
			Serial.print("curl --fail-with-body -F 'file=@hello.txt' http://");
			Serial.print(WiFi.localIP());
			Serial.println(UploadRoute);
		}
	});

	WiFi.persistent(false);
	WiFi.mode(WIFI_STA);
	WiFi.begin(WifiSsid, WifiPassword);
	http.begin();
}

// Handle incoming requests and track Wi-Fi connectivity.
void loop() {
	http.tick();
	delay(1);
}
