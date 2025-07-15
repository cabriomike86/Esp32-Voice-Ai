#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <driver/i2s.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include "Audio.h"


// OLED Display Setup
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Audio Hardware Setup
#define I2S_BCK 26    // MAX98357 BCK PIN
#define I2S_DOUT 25   // MAX98357 DIN PIN
#define I2S_LRC   22  // MAX98357 LRC PIN

#define I2S_SD 12     // MAX98357 shutdown pin

#define I2S_SCK 33    // INMP441 SCK PIN
#define I2S_WS 27     // INMP441 WS Pin
#define I2S_DIN 34    // INMP441 data pin
#define BUTTON_PIN 4 // Record button
#define CONFIG_PIN 14 // Boot button for config mode

// SD Card Setup
//#define REASSIGN_PINS
int sck = 18;
int miso = 19;
int mosi = 23;
int cs = 5;
#define SD_CS_PIN 5  // SD Card Chip Select
// SD Card Setup
#define SD_CS_PIN 5
#define SPI_MOSI_PIN      23 
#define SPI_MISO_PIN      19
#define SPI_SCK_PIN       18

// EEPROM Settings
#define EEPROM_SIZE 2048
#define WIFI_CONFIG_MAGIC 0x55AA
#define WIFI_MAX_NETWORKS 3
#define WIFI_CRED_MAX_LEN 32
#define API_KEY_LEN 64

typedef struct {
  uint16_t magic;
  char ssids[WIFI_MAX_NETWORKS][WIFI_CRED_MAX_LEN];
  char passwords[WIFI_MAX_NETWORKS][WIFI_CRED_MAX_LEN];
  char googleSpeechApiKey[API_KEY_LEN] = "AIzaSyDIYlKX1iuUK3bVRa6g6YV_ZnEr3_2lNl8";
  char googleTtsApiKey[API_KEY_LEN];
  char geminiApiKey[API_KEY_LEN];
} DeviceConfig;

// Function declarations
void displayStatus(const String& message);
void setError(const String& message);
size_t calculateDecodedSize(const char* base64String);
int base64_decode(const char* input, uint8_t* output);
bool isBase64(unsigned char c);
String base64_encode(const uint8_t* data, size_t input_length);

void loadConfig();
void saveConfig();
void enterConfigMode();
void setupAudioHardware();
void connectToWiFi();
void startRecording();
void stopRecording();
void processSpeech();
void queryGemini(const String& query);
void textToSpeech(const String& text);
void playAudio(const char* filename);

// Web Server
WebServer server(80);
WiFiMulti wifiMulti;
DeviceConfig deviceConfig;

// Audio Settings
const int SAMPLE_RATE = 44100;
const int RECORD_DURATION = 5000; // 5 seconds
uint8_t* audioBuffer = nullptr;
size_t audioBufferSize = 0;
bool isPlayingAudio = false;

// SD file for recording and playback
File audioFile;

// State machine
enum State {
  STATE_INIT,
  STATE_WIFI_CONFIG,
  STATE_WIFI_CONNECTING,
  STATE_WIFI_CONNECTED,
  STATE_READY,
  STATE_RECORDING,
  STATE_PROCESSING_SPEECH,
  STATE_QUERYING_AI,
  STATE_PROCESSING_TTS,
  STATE_PLAYING,
  STATE_ERROR
};
State currentState = STATE_INIT;
String errorMessage = "";

bool isConfigModeActive = false;

void setup() {
  Serial.begin(115200);
  
  // Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    while(1);
  }
  displayStatus("Booting...");
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadConfig();

  // Initialize SPI with custom pins
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, SD_CS_PIN);

// Initialize SD card
#ifdef REASSIGN_PINS
  //SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, SPI_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
#else
  if (!SD.begin(SD_CS_PIN)) {
#endif
    Serial.println("Card Mount Failed");
    setError("SD Card Init Failed");
    // Halt further operation since SD card is critical for recording/playback
    while (true) {
      delay(1000);
    }
  } else {
    Serial.println("SD Card Initialized");
  Serial.println("SD Card Initialized successfully");
  displayStatus("SD Card Ready");
  }
  // Initialize hardware
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(CONFIG_PIN, INPUT_PULLUP);
  pinMode(I2S_SD, OUTPUT);
#ifdef DISABLE_AUDIO_OUTPUT_ON_BOOT
  digitalWrite(I2S_SD, LOW); // Disable amplifier on boot
#else
  digitalWrite(I2S_SD, HIGH); // Enable amplifier
#endif

  // Check for config mode
  if (digitalRead(CONFIG_PIN) == LOW && !isConfigModeActive) {
    enterConfigMode();
    return;
  }

  setupAudioHardware();
  // Removed automatic WiFi connect on boot to wait for WiFi selection via WiFi manager
   connectToWiFi();
}

void loop() {
  if (currentState == STATE_WIFI_CONFIG) {
    server.handleClient();
    return;
  }

  static unsigned long recordStartTime = 0;
  static unsigned long stateEnterTime = 0;
  
  // State machine
  // Long-press detection for config mode
  static unsigned long configButtonPressTime = 0;
  static bool configButtonWasPressed = false;
  if (digitalRead(CONFIG_PIN) == LOW) {
    if (!configButtonWasPressed) {
      configButtonPressTime = millis();
      configButtonWasPressed = true;
    } else if (millis() - configButtonPressTime > 3000) { // 3 seconds hold
      displayStatus("Entering WiFi Manager...");
      delay(500);
      if (!isConfigModeActive) {
        enterConfigMode();
      }
      return;
    }
  } else {
    configButtonWasPressed = false;
  }

  switch(currentState) {
    case STATE_INIT:
      break;
    case STATE_WIFI_CONNECTING: {
      static int currentNetworkIndex = 0;
      if(wifiMulti.run() == WL_CONNECTED) {
        displayStatus("WiFi Connected to network #" + String(currentNetworkIndex + 1));
        Serial.print("Connected to: ");
        Serial.println(WiFi.SSID());
        currentState = STATE_WIFI_CONNECTED;
        stateEnterTime = millis();
      } else if (millis() > stateEnterTime + 30000) { // 30s timeout
        enterConfigMode();
      } else {
        // Update currentNetworkIndex based on which network is currently connected or being tried
        // Note: WiFiMulti does not expose current index, so this is a best effort
        currentNetworkIndex = (currentNetworkIndex + 1) % WIFI_MAX_NETWORKS;
      }
      break;
    }
    case STATE_WIFI_CONNECTED:
      if (millis() > stateEnterTime + 2000) { // Brief display of connection status
        displayStatus("Ready\nPress to record");
        currentState = STATE_READY;
      }
      break;
    case STATE_READY: {
      static unsigned long lastButtonPress = 0;
      if(digitalRead(BUTTON_PIN) == LOW) {
        unsigned long now = millis();
        if (now - lastButtonPress > 200) { // 200ms debounce
          displayStatus("Recording...");
          currentState = STATE_RECORDING;
          recordStartTime = now;
          startRecording();
          lastButtonPress = now;
        }
      }
      break;
    }
    case STATE_RECORDING:
      if(millis() - recordStartTime >= RECORD_DURATION) {
        stopRecording();
        displayStatus("Processing speech...");
        currentState = STATE_PROCESSING_SPEECH;
        processSpeech();
      } else {
        // During recording, read audio data and write to SD file
        size_t bytes_read = 0;
        uint8_t buffer[512];
        if (audioFile) {
          esp_err_t result = i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
          if (result == ESP_OK && bytes_read > 0) {
            audioFile.write(buffer, bytes_read);
            audioFile.flush();
          }
        }
      }
      break;
    case STATE_PROCESSING_SPEECH:
      break;
    case STATE_QUERYING_AI:
      break;
    case STATE_PROCESSING_TTS:
      break;
    case STATE_PLAYING:
      if(!isPlayingAudio) {
        currentState = STATE_READY;
        displayStatus("Ready\nPress to record");
      }
      break;
    case STATE_ERROR:
      if (millis() > stateEnterTime + 5000) { // Show error for 5 seconds
        currentState = STATE_READY;
        displayStatus("Ready\nPress to record");
      }
      break;
  }
  delay(10);
}

//========================================
// Configuration Functions
//========================================

void loadConfig() {
  EEPROM.get(0, deviceConfig);
  if (deviceConfig.magic != WIFI_CONFIG_MAGIC) {
    // Initialize with defaults
    memset(&deviceConfig, 0, sizeof(deviceConfig));
    deviceConfig.magic = WIFI_CONFIG_MAGIC;
    saveConfig();
  }
}

void saveConfig() {
  EEPROM.put(0, deviceConfig);
  EEPROM.commit();
}

void enterConfigMode() {
  currentState = STATE_WIFI_CONFIG;
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-VoiceAI");

  // Removed event handler for client connection to WiFi AP due to compilation errors and user request

  displayStatus("Config Mode\nConnect to:\nESP32-VoiceAI\nThen visit:\n192.168.4.1");
  
  server.on("/", HTTP_GET, []() {
    String html = R"=====(
    <html><head><title>ESP32 Voice Assistant</title>
    <style>
      body { font-family: Arial; margin: 20px; }
      h1 { color: #444; }
      form { max-width: 500px; }
      input { width: 100%; padding: 8px; margin: 5px 0 15px; box-sizing: border-box; }
      input[type="submit"] { background: #4CAF50; color: white; border: none; padding: 12px; }
      button { padding: 10px 20px; margin: 5px; font-size: 16px; }
      #testResult { margin-top: 15px; font-weight: bold; }
    </style>
    <script>
      function testMic() {
        fetch('/test/mic').then(response => response.text()).then(data => {
          document.getElementById('testResult').innerText = data;
        });
      }
      function testAudio() {
        fetch('/test/audio').then(response => response.text()).then(data => {
          document.getElementById('testResult').innerText = data;
        });
      }
    </script>
    </head><body>
    <h1>ESP32 Voice Assistant Setup</h1>
    <form method='post' action='/save'>
    <h3>WiFi Networks</h3>
    )=====";
    
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
      html += "<input type='text' name='ssid" + String(i+1) + "' placeholder='SSID " + String(i+1) + "' value='" + String(deviceConfig.ssids[i]) + "'><br>";
      html += "<input type='password' name='pass" + String(i+1) + "' placeholder='Password " + String(i+1) + "'><br>";
    }
    
    html += R"=====(
    <h3>API Keys</h3>
    <input type='text' name='speech' placeholder='Google Speech API Key' value=')=====";
    html += String(deviceConfig.googleSpeechApiKey);
    html += R"=====('><br>
    <input type='text' name='tts' placeholder='Google TTS API Key' value=')=====";
    html += String(deviceConfig.googleTtsApiKey);
    html += R"=====('><br>
    <input type='text' name='gemini' placeholder='Gemini API Key' value=')=====";
    html += String(deviceConfig.geminiApiKey);
    html += R"=====('><br>
    <input type='submit' value='Save & Reboot'>
    </form>
    <h3>Test Functions</h3>
    <button onclick='testMic()'>Test Microphone</button>
    <button onclick='testAudio()'>Test Audio Output</button>
    <div id='testResult'></div>
    </body></html>
    )=====";
    
    server.send(200, "text/html", html);
  });
  
  server.on("/save", HTTP_POST, []() {
    // Save WiFi credentials
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
      if (server.hasArg("ssid" + String(i+1))) {
        strncpy(deviceConfig.ssids[i], server.arg("ssid" + String(i+1)).c_str(), WIFI_CRED_MAX_LEN);
      }
      if (server.hasArg("pass" + String(i+1))) {
        strncpy(deviceConfig.passwords[i], server.arg("pass" + String(i+1)).c_str(), WIFI_CRED_MAX_LEN);
      }
    }
    
    // Save API keys
    if (server.hasArg("speech")) strncpy(deviceConfig.googleSpeechApiKey, server.arg("speech").c_str(), API_KEY_LEN);
    if (server.hasArg("tts")) strncpy(deviceConfig.googleTtsApiKey, server.arg("tts").c_str(), API_KEY_LEN);
    if (server.hasArg("gemini")) strncpy(deviceConfig.geminiApiKey, server.arg("gemini").c_str(), API_KEY_LEN);
    
    saveConfig();
    server.send(200, "text/plain", "Configuration saved. Connecting to WiFi...");
    // Connect to WiFi after saving config
    connectToWiFi();
  });

  // Test microphone endpoint
  server.on("/test/mic", HTTP_GET, []() {
    displayStatus("Testing mic... Please wait: recording 2 seconds");
    startRecording();
    delay(5000); // Record 2 seconds
    stopRecording();
    displayStatus("Playing back test recording... Please wait");
    playAudio("/recording.wav");
    server.send(200, "text/plain", "Microphone test completed.");
  });

  // Test audio output endpoint
  server.on("/test/audio", HTTP_GET, []() {
    displayStatus("Testing audio output... Please wait");
    // Play a predefined test tone or audio file
    // For simplicity, play the last recorded audio if exists
    if (SD.exists("recording.wav")) {
      playAudio("/recording.wav");
      server.send(200, "text/plain", "Audio output test completed.");
    } else {
      server.send(200, "text/plain", "No test audio available.");
    }
  });
  
  server.begin();
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  displayStatus("Connecting WiFi...");
  currentState = STATE_WIFI_CONNECTING;
  
  // Clear existing networks
  wifiMulti = WiFiMulti(); // Reset the WiFiMulti object
  
  for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
    if (strlen(deviceConfig.ssids[i]) > 0) {
      wifiMulti.addAP(deviceConfig.ssids[i], deviceConfig.passwords[i]);
      Serial.printf("Added WiFi: %s\n", deviceConfig.ssids[i]);
    }
  }
}

//========================================
// Audio Functions
//========================================

#include "driver/i2s.h"

void setupAudioHardware() {
  Serial.println("Starting audio hardware setup");

  // I2S configuration for microphone (RX)
  i2s_config_t i2s_mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 16,
    .dma_buf_len = 60,
    .use_apll = true,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

i2s_pin_config_t mic_pins = {
    .bck_io_num = I2S_SCK,  // bck_io_num
    .ws_io_num = I2S_WS,   // ws_io_num
    .data_out_num = I2S_PIN_NO_CHANGE, // data_out_num
    .data_in_num = I2S_DIN,  // data_in_num
};

  // Install and start I2S driver for microphone
  // Remove uninstall call to avoid error if driver not installed
  // i2s_driver_uninstall(I2S_NUM_0);
  i2s_driver_install(I2S_NUM_0, &i2s_mic_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &mic_pins);

  // I2S configuration for amplifier (TX)
  i2s_config_t i2s_amp_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 16,
    .dma_buf_len = 60,
    .use_apll = true,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

i2s_pin_config_t amp_pins = {
    .bck_io_num = I2S_BCK,  // bck_io_num
    .ws_io_num = I2S_WS,   // ws_io_num
    .data_out_num = I2S_DOUT, // data_out_num
    .data_in_num = I2S_PIN_NO_CHANGE,  // data_in_num
};

  // Install and start I2S driver for amplifier
  // Remove uninstall call to avoid error if driver not installed
  // i2s_driver_uninstall(I2S_NUM_1);
  i2s_driver_install(I2S_NUM_1, &i2s_amp_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &amp_pins);

  Serial.println("Audio hardware initialized");
}

void writeWavHeader(File &file, uint32_t dataLength) {
  // WAV header is 44 bytes
  uint8_t header[44];

  // RIFF chunk descriptor
  memcpy(header, "RIFF", 4);
  uint32_t chunkSize = 36 + dataLength;
  header[4] = (chunkSize & 0xff);
  header[5] = ((chunkSize >> 8) & 0xff);
  header[6] = ((chunkSize >> 16) & 0xff);
  header[7] = ((chunkSize >> 24) & 0xff);
  memcpy(header + 8, "WAVE", 4);

  // fmt subchunk
  memcpy(header + 12, "fmt ", 4);
  header[16] = 16; // Subchunk1Size for PCM
  header[17] = 0;
  header[18] = 0;
  header[19] = 0;
  header[20] = 1; // AudioFormat PCM = 1
  header[21] = 0;
  header[22] = 1; // NumChannels = 1 (mono)
  header[23] = 0;
  header[24] = (SAMPLE_RATE & 0xff);
  header[25] = ((SAMPLE_RATE >> 8) & 0xff);
  header[26] = ((SAMPLE_RATE >> 16) & 0xff);
  header[27] = ((SAMPLE_RATE >> 24) & 0xff);
  uint32_t byteRate = SAMPLE_RATE * 2; // SampleRate * NumChannels * BitsPerSample/8
  header[28] = (byteRate & 0xff);
  header[29] = ((byteRate >> 8) & 0xff);
  header[30] = ((byteRate >> 16) & 0xff);
  header[31] = ((byteRate >> 24) & 0xff);
  uint16_t blockAlign = 2; // NumChannels * BitsPerSample/8
  header[32] = (blockAlign & 0xff);
  header[33] = ((blockAlign >> 8) & 0xff);
  uint16_t bitsPerSample = 16;
  header[34] = (bitsPerSample & 0xff);
  header[35] = ((bitsPerSample >> 8) & 0xff);

  // data subchunk
  memcpy(header + 36, "data", 4);
  header[40] = (dataLength & 0xff);
  header[41] = ((dataLength >> 8) & 0xff);
  header[42] = ((dataLength >> 16) & 0xff);
  header[43] = ((dataLength >> 24) & 0xff);

  file.seek(0);
  file.write(header, 44);
}

void startRecording() {
  // Close any previously open file
  if (audioFile) {
    audioFile.close();
  }
  // Open file for writing WAV data with .raw extension
  audioFile = SD.open("/recording.wav", FILE_WRITE);
  if (!audioFile) {
    setError("Failed to open file for recording");
    return;
  }
  // Write placeholder WAV header (44 bytes)
  uint8_t emptyHeader[44] = {0};
  audioFile.write(emptyHeader, 44);
  audioFile.flush();

  Serial.println("Recording started");
}

void stopRecording() {
  if (audioFile) {
    audioFile.flush();
    uint32_t fileSize = audioFile.size();
    uint32_t dataLength = fileSize - 44;
    writeWavHeader(audioFile, dataLength);
    audioFile.close();
    Serial.println("Recording stopped");
  } else {
    Serial.println("No recording file open");
  }
}

//========================================
// Cloud Services
//========================================

void processSpeech() {
  // Read recorded audio file from SD card
  if (!SD.exists("/recording.wav")) {
    setError("No audio file found");
    return;
  }

  File file = SD.open("/recording.wav", FILE_READ);
  if (!file) {
    setError("Failed to open audio file");
    return;
  }

  // Read entire file into buffer for base64 encoding
  size_t fileSize = file.size();
  uint8_t* fileBuffer = (uint8_t*)malloc(fileSize);
  if (!fileBuffer) {
    setError("Memory allocation failed");
    file.close();
    return;
  }

  size_t bytesRead = file.read(fileBuffer, fileSize);
  file.close();

  if (bytesRead != fileSize) {
    setError("Failed to read complete audio file");
    free(fileBuffer);
    return;
  }

  String audioBase64 = base64_encode(fileBuffer, fileSize);
  free(fileBuffer);

  Serial.print("Audio base64 length: ");
  Serial.println(audioBase64.length());
  if (audioBase64.length() == 0) {
    setError("Audio data is empty");
    return;
  }

  // Stream file and encode base64 in chunks to avoid large memory usage
  const size_t chunkSize = 512;
  uint8_t buffer[chunkSize];

  while ((bytesRead = file.read(buffer, chunkSize)) > 0) {
    audioBase64 += base64_encode(buffer, bytesRead);
  }
  file.close();

  Serial.print("Audio base64 length: ");
  Serial.println(audioBase64.length());
  if (audioBase64.length() == 0) {
    setError("Audio data is empty");
    return;
  }

  HTTPClient http;
  http.begin("https://speech.googleapis.com/v1/speech:recognize?key=" + String(deviceConfig.googleSpeechApiKey));
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"config\":{\"encoding\":\"LINEAR16\",\"sampleRateHertz\":" + String(SAMPLE_RATE) +
                   ",\"languageCode\":\"en-US\"},\"audio\":{\"content\":\"" + audioBase64 + "\"}}";

  Serial.print("Payload: ");
  Serial.println(payload);

  int httpCode = http.POST(payload);

  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, response);

    if (!error && doc["results"].is<JsonArray>()) {
      const char* transcript = doc["results"][0]["alternatives"][0]["transcript"];
      Serial.print("Transcript: ");
      Serial.println(transcript);

      displayStatus("Querying AI...");
      currentState = STATE_QUERYING_AI;
      queryGemini(transcript);
    } else if (error) {
      setError("JSON Parse Err: " + String(error.c_str()));
    } else {
      setError("No transcription");
    }
  } else {
    setError("Speech API: " + String(httpCode));
  }

  http.end();
}

void queryGemini(const String& query) {
  HTTPClient http;
  http.begin("https://generativelanguage.googleapis.com/v1beta/models/gemini-pro:generateContent?key=" + String(deviceConfig.geminiApiKey));
  http.addHeader("Content-Type", "application/json");
  
  String payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + query + "\"}]}]}";
  
  int httpCode = http.POST(payload);
  
  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error && doc.containsKey("candidates")) {
      const char* aiResponse = doc["candidates"][0]["content"]["parts"][0]["text"];
      Serial.print("AI Response: ");
      Serial.println(aiResponse);
      
      displayStatus("Converting to speech...");
      currentState = STATE_PROCESSING_TTS;
      textToSpeech(aiResponse);
    } else if (error) {
      setError("JSON Parse Err: " + String(error.c_str()));
    }
  } else {
    setError("Gemini API: " + String(httpCode));
  }
  
  http.end();
}

void textToSpeech(const String& text) {
    HTTPClient http;
    http.begin("https://texttospeech.googleapis.com/v1/text:synthesize?key=" + String(deviceConfig.googleTtsApiKey));
    http.addHeader("Content-Type", "application/json");
    
    String payload = "{\"input\":{\"text\":\"" + text + "\"},\"voice\":{\"languageCode\":\"en-US\",\"name\":\"en-US-Wavenet-D\"},\"audioConfig\":{\"audioEncoding\":\"LINEAR16\",\"speakingRate\":1.0,\"pitch\":0.0}}";
    
    int httpCode = http.POST(payload);
    
    if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, response);
        
        if (!error && doc.containsKey("audioContent")) {
            const char* audioContent = doc["audioContent"];
            size_t decodedSize = calculateDecodedSize(audioContent);
            uint8_t* decodedAudio = (uint8_t*)malloc(decodedSize);
            
            // Use the custom base64 decode function
            int bytesDecoded = base64_decode(audioContent, decodedAudio);
            
            displayStatus("Playing response...");
            currentState = STATE_PLAYING;
            // Save decoded audio to SD card for playback
            if (audioFile) {
              audioFile.close();
            }
            audioFile = SD.open("/response.raw", FILE_WRITE);
            if (audioFile) {
              audioFile.write(decodedAudio, bytesDecoded);
              audioFile.close();
              playAudio("/response.raw");
            } else {
              setError("Failed to open response file");
            }
            
            free(decodedAudio);
        } else if (error) {
            setError("JSON Parse Err: " + String(error.c_str()));
        }
    } else {
        setError("TTS API: " + String(httpCode));
    }
    
    http.end();
}

void playAudio(const char* filename) {
  isPlayingAudio = true;
  
  File file = SD.open(filename, FILE_READ);
  if (!file) {
    setError("Failed to open audio file");
    isPlayingAudio = false;
    return;
  }
  
  const size_t bufferSize = 512;
  uint8_t buffer[bufferSize];
  size_t bytesRead = 0;
  size_t bytesWritten = 0;
  
  while ((bytesRead = file.read(buffer, bufferSize)) > 0) {
    i2s_write(I2S_NUM_1, buffer, bytesRead, &bytesWritten, portMAX_DELAY);
  }
  
  file.close();
  isPlayingAudio = false;
}

bool isAudioPlaying() {
  return isPlayingAudio;
}

//========================================
// Base64 Functions
//========================================

const char* base64_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

bool isBase64(unsigned char c) {
 return (isalnum(c) || (c == '+') || (c == '/'));
}

int base64_decode(const char* input, uint8_t* output) {
    int in_len = strlen(input);
    int i = 0, j = 0, in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];

    while (in_len-- && (input[in_] != '=') && isBase64(input[in_])) {
        char_array_4[i++] = input[in_]; in_++;
        if (i ==4) {
            for (i = 0; i < 4; i++)
                char_array_4[i] = strchr(base64_chars, char_array_4[i]) - base64_chars;
            char_array_3[0] = (char_array_4[0] << 2) + (char_array_4[1] >> 4);
            char_array_3[1] = (char_array_4[1] << 4) + (char_array_4[2] >> 2);
            char_array_3[2] = (char_array_4[2] << 6) + char_array_4[3];
            for (i = 0; (i < 3); i++)
                output[j++] = char_array_3[i];
            i = 0;
        }
    }

    if (i) {
        for (int k = i; k < 4; k++)
            char_array_4[k] = 0;
        for (int k = 0; k < 4; k++)
            char_array_4[k] = strchr(base64_chars, char_array_4[k]) - base64_chars;
        char_array_3[0] = (char_array_4[0] << 2) + (char_array_4[1] >> 4);
        char_array_3[1] = (char_array_4[1] << 4) + (char_array_4[2] >> 2);
        for (int k = 0; (k < i - 1); k++) output[j++] = char_array_3[k];
    }

    return j;
}

String base64_encode(const uint8_t* data, size_t input_length) {
    String output;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    while (input_length--) {
        char_array_3[i++] = *(data++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; (i < 4); i++)
                output += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (int k = i; k < 3; k++)
            char_array_3[k] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        for (int k = 0; (k < i + 1); k++)
            output += base64_chars[char_array_4[k]];

        while ((i++ < 3))
            output += '=';
    }

    return output;
}

size_t calculateDecodedSize(const char* base64String) {
    size_t length = strlen(base64String);
    size_t padding = 0;

    if (length > 0 && base64String[length - 1] == '=') padding++;
    if (length > 1 && base64String[length - 2] == '=') padding++;

    return (length * 3) / 4 - padding;
}

void displayStatus(const String& message) {
    Serial.print("[STATUS] ");
    Serial.println(message);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(message);
    display.display();
}

void setError(const String& message) {
    errorMessage = message;
    displayStatus("Error: " + message);
    currentState = STATE_ERROR;
}
