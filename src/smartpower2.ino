#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WebSocketsServer.h>
#include <ESP8266WebServer.h>
#include <Hash.h>
#include <FS.h>
#include <Wire.h>
#include <SimpleTimer.h>
#include <mcp4652.h>
#include <LiquidCrystal_I2C.h>

#define USE_SERIAL Serial

#define ON	0
#define OFF	1
#define HOME    0
#define SETTINGS    1

#define POWER		D6
#define POWERLED	D1
#define BTN_ONOFF	D7
#define I2C_SDA		D2
#define I2C_SCL		D5

#define SET_DEFAULT_VOLTAGE	'v'
#define SET_VOLTAGE			'w'
#define SAVE_NETWORKS		'n'
#define CMD_ONOFF			'o'
#define SET_AUTORUN			'a'
#define PAGE_STATE			'p'
#define DATA_PVI			'd'
#define MEASUREWATTHOUR		'm'

uint8_t onoff = OFF;
unsigned char measureWh;
float setVoltage;
unsigned char connectedWeb;
unsigned char connectedLCD;
unsigned char timerId;
unsigned char timerId2;
unsigned char autorun;

unsigned char D4state;
unsigned char D1state;

char ssid[20];
char password[20];
unsigned int ipaddr[4];

float volt;
float ampere;
float watt;
double watth;

#define MAX_SRV_CLIENTS 1

ESP8266WebServer server;
WebSocketsServer webSocket = WebSocketsServer(81);
IPAddress ip = IPAddress(192, 168, 4, 1);

SimpleTimer timer;
SimpleTimer timer2;
// lcd slave address are 0x27 or 0x3f
LiquidCrystal_I2C lcd(0x27, 16, 2);

struct client_sp2 {
    uint8_t connected;
    uint8_t page;
};

struct client_sp2 client_sp2[5];

void setup() {
    USE_SERIAL.begin(115200);
    USE_SERIAL.setDebugOutput(true);
    pinMode(POWERLED, OUTPUT);

    Wire.begin(I2C_SDA, I2C_SCL);
    mcp4652_init();
    ina231_configure();

    pinMode(BTN_ONOFF, INPUT);
    attachInterrupt(digitalPinToInterrupt(BTN_ONOFF), pinChanged, CHANGE);

    for(uint8_t t = 4; t > 0; t--) {
        USE_SERIAL.printf("[SETUP] BOOT WAIT %d...\n\r", t);
        USE_SERIAL.flush();
        delay(1000);
    }

    SPIFFS.begin();
    {
        Dir dir = SPIFFS.openDir("/");
        while (dir.next()) {
            String fileName = dir.fileName();
            size_t fileSize = dir.fileSize();
            USE_SERIAL.printf("FS File: %s, size: %s\n\r",
            fileName.c_str(), formatBytes(fileSize).c_str());
        }
        USE_SERIAL.printf("\n\r");
    }

    initSmartPower();
	lcd_status();

	webserver_init();

    timerId = timer.setInterval(1000, handler);
}

void loop() {
    server.handleClient();
    webSocket.loop();
    timer.run();
}

void webserver_init(void)
{
    IPAddress gateway(ip[0], ip[1], ip[2], ip[3]);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(ip, gateway, subnet);

    WiFi.softAP(ssid, password);
    USE_SERIAL.println("");
    USE_SERIAL.print("Connected! IP address: ");
    USE_SERIAL.println(WiFi.softAPIP());


    // start webSocket server
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);

    server.on("/list", HTTP_GET, handleFileList);

    //use it to load content from SPIFFS
    server.onNotFound([](){
        if(!handleFileRead(server.uri()))
        server.send(404, "text/plain", "FileNotFound");
    });
    server.begin();

    USE_SERIAL.println("HTTP server started");
}

//format bytes
String formatBytes(size_t bytes){
    if (bytes < 1024){
        return String(bytes)+"B";
    } else if(bytes < (1024 * 1024)){
        return String(bytes/1024.0)+"KB";
    } else if(bytes < (1024 * 1024 * 1024)){
        return String(bytes/1024.0/1024.0)+"MB";
    } else {
        return String(bytes/1024.0/1024.0/1024.0)+"GB";
    }
}

String getContentType(String filename) {
    if(server.hasArg("download")) return "application/octet-stream";
    else if(filename.endsWith(".htm")) return "text/html";
    else if(filename.endsWith(".html")) return "text/html";
    else if(filename.endsWith(".js")) return "application/javascript";
    else if(filename.endsWith(".css")) return "text/css";
    else if(filename.endsWith(".gif")) return "image/gif";
    else if(filename.endsWith(".ico")) return "image/x-icon";
    return "text/plain";
}

bool handleFileRead(String path) {
    USE_SERIAL.println("handleFileRead: " + path);
    if (path.endsWith("/")) {
        path += "index.html";
    }
    String contentType = getContentType(path);
    String pathWithGz = path + ".gz";
    if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
        if (SPIFFS.exists(pathWithGz))
        path += ".gz";
        File file = SPIFFS.open(path, "r");
        size_t sent = server.streamFile(file, contentType);
        file.close();
        return true;
    }
    Serial.println("False!");
    return false;
}

void handleFileList() {
    if(!server.hasArg("dir")) {server.send(500, "text/plain", "BAD ARGS"); return;}
    String path = server.arg("dir");
    Serial.println("handleFileList: " + path);
    Dir dir = SPIFFS.openDir(path);
    path = String();

    String output = "[";
    while(dir.next()) {
        File entry = dir.openFile("r");
        if (output != "[") output += ',';
        bool isDir = false;
        output += "{\"type\":\"";
        output += (isDir)?"dir":"file";
        output += "\",\"name\":\"";
        output += String(entry.name()).substring(1);
        output += "\"}";
        entry.close();
    }

    output += "]";
    server.send(200, "text/json", output);
}

void handleClientData(uint8_t num, String data)
{
    Serial.println(data);
    switch (data.charAt(0)) {
	case PAGE_STATE:
		client_sp2[num].page = data.charAt(1)-48;
		sendStatus(num, data.charAt(1)-48);
		break;
	case CMD_ONOFF:
		onoff = data.substring(1).toInt();
		digitalWrite(POWER, onoff);
		digitalWrite(POWERLED, LOW);
		watth = 0;
		send_data_to_clients(String(CMD_ONOFF) + onoff, HOME, num);
		break;
	case SET_VOLTAGE:
        setVoltage = data.substring(1).toFloat();
        mcp4652_write(WRITE_WIPER0, quadraticRegression(setVoltage));
		send_data_to_clients(String(SET_VOLTAGE) + setVoltage, HOME, num);
        break;
	case SET_DEFAULT_VOLTAGE: {
        File f = SPIFFS.open("/txt/settings.txt", "r+");
        f.seek(0, SeekSet);
        f.findUntil("voltage", "\n\r");
        f.seek(1, SeekCur);
        f.print(setVoltage);
        f.close();
		break;
        }
	case SET_AUTORUN: {
		if (autorun == data.substring(1).toInt()) {
			break;
		}
		autorun = data.substring(1).toInt();

		File f = SPIFFS.open("/txt/settings.txt", "r+");
		f.seek(0, SeekSet);
		f.findUntil("autorun", "\n\r");
		f.seek(1, SeekCur);
		f.print(autorun);
		f.close();

		send_data_to_clients(String(SET_AUTORUN) + autorun, SETTINGS, num);
		break;
	}
	case SAVE_NETWORKS: {
            String _ssid = data.substring(1, data.indexOf(','));
            data.remove(1, data.indexOf(','));
            String _ipaddr = data.substring(1, data.indexOf(','));
            data.remove(1, data.indexOf(','));
            String _password = data.substring(1, data.indexOf(','));
            if ((String(ssid) != _ssid) || (ip.toString() != _ipaddr) ||
                                (String(password) != _password)) {
                File f = SPIFFS.open("/js/settings.js", "w");
                f.println("ssid=\"" + _ssid + "\"");
                f.println("ipaddr=\"" + _ipaddr + "\"");
                f.println("passwd=\"" + _password + "\"");
                f.flush();
                f.close();
                readNetworkConfig();
                for (int i = 0; i < 5; i++) {
                    if (client_sp2[i].connected && client_sp2[i].page && (num != i))
                        sendStatus(i, 1);
                }
            }

            break;
        }
	case MEASUREWATTHOUR:
		measureWh = data.substring(1).toInt();
		send_data_to_clients(String(MEASUREWATTHOUR) + measureWh, HOME, num);
		break;
	}
}

void fs_init(void)
{
    File f = SPIFFS.open("/js/settings.js", "w");
    f.println("ssid=\"sp2\"");
    f.println("ipaddr=\"192.168.4.1\"");
    f.println("passwd=\"12345678\"");
    f.flush();
    f.close();
    readNetworkConfig();

    File f2 = SPIFFS.open("/txt/settings.txt", "w");
    f2.println("autorun=0");
    f2.print("voltage=5.00");
    f2.flush();
    f2.close();
    readHWSettings();
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght) {
    switch(type) {
    case WStype_DISCONNECTED :
        USE_SERIAL.printf("[%u] Disconnected!\n\r", num);
        client_sp2[num].connected = 0;
        connectedWeb = 0;
        break;
    case WStype_CONNECTED : {
            IPAddress ip = webSocket.remoteIP(num);
            USE_SERIAL.printf("[%u] Connected from %d.%d.%d.%d url: %s\n\r", num, ip[0], ip[1], ip[2], ip[3], payload);
            client_sp2[num].connected = 1;
            connectedWeb = 1;
        }
        break;
    case WStype_TEXT : {
            handleClientData(num, String((char *)&payload[0]));
        }
        break;
    case WStype_BIN :
        USE_SERIAL.printf("[%u] get binary lenght: %u\n\r", num, lenght);
        hexdump(payload, lenght);
        break;
    }
}

void readHWSettings(void)
{
    File f = SPIFFS.open("/txt/settings.txt", "r");

    f.seek(0, SeekSet);
    f.findUntil("autorun", "\n\r");
    f.seek(1, SeekCur);
    autorun = f.readStringUntil('\n').toInt();

    f.findUntil("voltage", "\n\r");
    f.seek(1, SeekCur);
    setVoltage = f.readStringUntil('\n').toFloat();

    f.close();
}

void readNetworkConfig(void)
{
    File f = SPIFFS.open("/js/settings.js", "r");

    f.findUntil("ssid", "\n\r");

    f.seek(2, SeekCur);
    f.readStringUntil('"').toCharArray(ssid, 20);

    f.findUntil("ipaddr", "\n\r");
    f.seek(2, SeekCur);
    ipaddr[0] = f.readStringUntil('.').toInt();
    ipaddr[1] = f.readStringUntil('.').toInt();
    ipaddr[2] = f.readStringUntil('.').toInt();
    ipaddr[3] = f.readStringUntil('"').toInt();
    ip = IPAddress(ipaddr[0], ipaddr[1], ipaddr[2], ipaddr[3]);
    server =  ESP8266WebServer(ip, 80);

    f.findUntil("passwd", "\n\r");
    f.seek(2, SeekCur);
    f.readStringUntil('"').toCharArray(password, 20);

    f.close();
}

void initSmartPower(void)
{
    readHWSettings();
    onoff = !autorun;
    readNetworkConfig();

    mcp4652_write(WRITE_WIPER0, quadraticRegression(setVoltage));
    pinMode(POWER, OUTPUT);
    digitalWrite(POWER, onoff);

    pinMode(D4, OUTPUT);
    digitalWrite(D4, HIGH);
}

// lcd slave address are 0x27 or 0x3f
unsigned char lcdSlaveAddr;
int lcd_available(void)
{
	Wire.beginTransmission(0x27);
	if (!Wire.endTransmission()) {
		lcdSlaveAddr = 0x27;
		return lcdSlaveAddr;
	} else {
		Wire.beginTransmission(0x3f);
		if (!Wire.endTransmission()) {
			lcdSlaveAddr = 0x3f;
			return lcdSlaveAddr;
		}
	}

	lcdSlaveAddr = 0;

	return 0;
}

void lcd_status(void)
{
	if (lcd_available() > 0) {
		if (!connectedLCD) {
   			lcd = LiquidCrystal_I2C(lcdSlaveAddr, 16, 2);
		    lcd.init();
		    lcd.backlight();
		    connectedLCD = 1;
		}
	} else {
		connectedLCD = 0;
	}
}

void printPower_LCD(void)
{
	lcd.setCursor(0, 0);
	lcd.print(volt, 3);
	lcd.print(" V ");
	lcd.print(ampere, 3);
	lcd.print(" A  ");

	lcd.setCursor(0, 1);
	if (watt < 10) {
			lcd.print(watt, 3);
	} else {
			lcd.print(watt, 2);
	}
	lcd.print(" W ");

	if (watth < 10) {
			lcd.print(watth, 3);
	} else if (watth < 100) {
			lcd.print(watth, 2);
	} else {
			lcd.print(watth, 1);
	}
	lcd.print(" Wh ");
}

uint8_t cnt_ssid;
uint8_t cursor_lcd;
void printInfo_LCD(void)
{
	for (int i = 0; i < 30; i++) {
		if (ssid[i] == '\0') {
			cnt_ssid = i;
			break;
		}
	}

	lcd.setCursor(0, 0);
	lcd.print("SSID:");
	lcd.print(ssid);
	if (cnt_ssid < 11) {
		for (int i = 0; i < 11 - cnt_ssid; i++) {
			lcd.print(" ");
		}
	}

	lcd.setCursor(0, 1);
	lcd.print("IP:");
	lcd.print(ip.toString());
	lcd.print("     ");
}

void readPower(void)
{
        volt = ina231_read_voltage();
        watt = ina231_read_power();
        ampere = ina231_read_current();
}

void wifi_connection_status(void)
{
    if (WiFi.softAPgetStationNum() > 0) {
        if (connectedWeb) {
            D4state = !D4state;
        } else {
			D4state = LOW;
		}
    } else {
		D4state = HIGH;
    }
	digitalWrite(D4, D4state);
}

double a = 0.0000006562;
double b = 0.0022084236;
float c = 4.08;
int quadraticRegression(double volt)
{
    double d;
    double root;
    d = b * b -a*(c-volt);
    root = (-b + sqrt(d))/a;
    if (root < 0) {
        root = 0;
    } else if (root > 255) {
        root = 255;
    }
    return root;
}

unsigned long btnPress;
unsigned long btnRelese = 1;
unsigned long currtime;
unsigned char btnChanged;
unsigned char resetCnt;
unsigned char swlock;
void pinChanged()
{
    if ((millis() - currtime) > 30) {
        swlock = 0;
    }

    if (!swlock) {
        if (!digitalRead(BTN_ONOFF) && (btnPress == 0)) {
            swlock = 1;
            currtime = millis();
            btnPress = 1;
            btnRelese = 0;
        }
    }

    if (!swlock) {
        if (digitalRead(BTN_ONOFF) && (btnRelese == 0)) {
            swlock = 1;
            currtime = millis();
            btnRelese = 1;
            btnPress = 0;
            btnChanged = 1;
            onoff = !onoff;
            watth = 0;
            digitalWrite(POWER, onoff);
            digitalWrite(POWERLED, LOW);
        }
    }
}

void readSystemReset()
{
        if (!digitalRead(BTN_ONOFF) && (btnPress == 1)) {
		if (resetCnt++ > 5) {
			USE_SERIAL.println("System Reset!!");
			fs_init();
			resetCnt = 0;
		}
	} else {
		resetCnt = 0;
	}
}

void sendStatus(uint8_t num, uint8_t page)
{
    if (!page) {
        webSocket.sendTXT(num, String(CMD_ONOFF) + onoff);
        webSocket.sendTXT(num, String(SET_VOLTAGE) + setVoltage);
        webSocket.sendTXT(num, String(MEASUREWATTHOUR) + measureWh);
    } else if (page) {
        webSocket.sendTXT(num, String(SET_AUTORUN) + autorun);
        webSocket.sendTXT(num, String(SAVE_NETWORKS) + String(ssid) + "," +
                        ip.toString() + "," + String(password));
    }
}

void send_data_to_clients(String str, uint8_t page)
{
    for (int i = 0; i < 5; i++) {
        if (client_sp2[i].connected && (client_sp2[i].page == page))
            webSocket.sendTXT(i, str);
    }
}

void send_data_to_clients(String str, uint8_t page, uint8_t num)
{
    for (int i = 0; i < 5; i++) {
        if (client_sp2[i].connected && (client_sp2[i].page == page) && (num != i))
            webSocket.sendTXT(i, str);
    }
}

void handler(void)
{
	if (onoff == ON) {
		digitalWrite(POWERLED, D1state = !D1state);
		if (connectedLCD || connectedWeb) {
			readPower();
		}
	}

	if (connectedLCD) {
		if (onoff == ON)
			printPower_LCD();
		else if (onoff == OFF)
			printInfo_LCD();
	}

	if (connectedWeb) {
		if (btnChanged) {
			send_data_to_clients(String(CMD_ONOFF) + onoff, HOME);
			if (onoff == OFF) {
				measureWh = 0;
			}
			btnChanged = 0;
		}
		if (onoff == ON) {
			String data = String(DATA_PVI);
			data += String(watt, 3) + "," + String(volt) + "," + String(ampere);
			if (measureWh) {
				watth += watt;
			}
			data += "," + String(watth/3600, 3);
			send_data_to_clients(data, HOME);
		}
	}

	lcd_status();
	wifi_connection_status();
	readSystemReset();
}