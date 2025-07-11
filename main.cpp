#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <driver/i2s.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

// OLED Display Setup
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Audio Hardware Setup
#define I2S_SD 12   // MAX98357 shutdown pin
#define I2S_BCK 13
#define I2S_WS 25
#define I2S_DOUT 33
#define I2S_DIN 32  // INMP441 data pin
#define BUTTON_PIN 4 // Record button
#define CONFIG_PIN 0 // Boot button for config mode
#define SD_CS_PIN 5  // SD Card Chip Select

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
  char googleSpeechApiKey[API_KEY_LEN];
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

// Added missing function declarations to fix undeclared errors
void loadConfig();
void saveConfig();
void enterConfigMode();
void setupSDCard();
void setupAudioHardware();
void connectToWiFi();
void startRecording();
void stopRecording();
void processSpeech();
void queryGemini(const String& query);
void textToSpeech(const String& text);
void playAudio(const uint8_t* audioData, size_t size);

// Web Server
WebServer server(80);
WiFiMulti wifiMulti;
DeviceConfig deviceConfig;

// Audio Settings
const int SAMPLE_RATE = 16000;
const int RECORD_DURATION = 5000; // 5 seconds
uint8_t* audioBuffer = nullptr;
size_t audioBufferSize = 0;
bool isPlayingAudio = false;

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

void setup() {
  Serial.begin(9600);
  
  // Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    while(1);
  }
  displayStatus("Booting...");
  
  // Initialize SD Card
  setupSDCard();

  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadConfig();

  // Initialize hardware
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(CONFIG_PIN, INPUT_PULLUP);
  pinMode(I2S_SD, OUTPUT);
  digitalWrite(I2S_SD, HIGH); // Enable amplifier

  // Check for config mode
  if (digitalRead(CONFIG_PIN) == LOW) {
    enterConfigMode();
    return;
  }

  setupAudioHardware();
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
      enterConfigMode();
      return;
    }
  } else {
    configButtonWasPressed = false;
  }

  switch(currentState) {
    case STATE_INIT:
      break;
    case STATE_WIFI_CONNECTING:
      if(wifiMulti.run() == WL_CONNECTED) {
        displayStatus("WiFi Connected");
        Serial.print("Connected to: ");
        Serial.println(WiFi.SSID());
        currentState = STATE_WIFI_CONNECTED;
        stateEnterTime = millis();
      } else if (millis() > stateEnterTime + 30000) { // 30s timeout
        enterConfigMode();
      }
      break;
    case STATE_WIFI_CONNECTED:
      if (millis() > stateEnterTime + 2000) { // Brief display of connection status
        displayStatus("Ready\nPress to record");
        // Fetch and play short audio sample from web after WiFi connects
        Serial.println("Fetching and playing short audio sample after WiFi connect...");
        HTTPClient http;
        String url = "https://www2.cs.uic.edu/~i101/SoundFiles/StarWars60.wav";
        WiFiClientSecure *client = new WiFiClientSecure;
        client->setInsecure();
        http.begin(*client, url);
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
          WiFiClient* stream = http.getStreamPtr();
          if (!stream || !stream->available()) {
            Serial.println("Web audio stream unavailable");
            http.end();
            delete client;
            break;
          }
          // Skip WAV header (typically 44 bytes)
          for (int i = 0; i < 44 && stream->available(); i++) stream->read();
          const size_t bufSize = 4096;
          uint8_t* buf = (uint8_t*)malloc(bufSize);
          if (!buf) {
            Serial.println("Memory error for web audio buffer");
            http.end();
            delete client;
            break;
          }
          size_t totalRead = 0;
          unsigned long startTime = millis();
          while (stream->available() && millis() - startTime < 5000) {
            int len = stream->read(buf, bufSize);
            if (len > 0) {
              playAudio(buf, len);
              totalRead += len;
            }
          }
          free(buf);
          Serial.printf("Played %u bytes from web audio after WiFi connect.\n", totalRead);
        } else {
          Serial.printf("Failed to fetch web audio after WiFi connect. HTTP code: %d\n", httpCode);
        }
        http.end();
        delete client;
        currentState = STATE_READY;
      }
      break;
    case STATE_READY:
      if(digitalRead(BUTTON_PIN) == LOW) {
        delay(50); // Debounce
        if(digitalRead(BUTTON_PIN) == LOW) {
          displayStatus("Recording...");
          currentState = STATE_RECORDING;
          recordStartTime = millis();
          startRecording();
        }
      }
      break;
    case STATE_RECORDING:
      if(millis() - recordStartTime >= RECORD_DURATION) {
        stopRecording();
        displayStatus("Processing speech...");
        currentState = STATE_PROCESSING_SPEECH;
        processSpeech();
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
      .graph { width: 100%; height: 80px; background: #eee; border: 1px solid #ccc; margin: 10px 0; }
      .btn { background: #2196F3; color: white; border: none; padding: 10px 20px; margin: 5px; cursor: pointer; }
    </style>
    <script>
      function testRecord() {
        fetch('/test_record').then(r => r.json()).then(data => {
          let graph = document.getElementById('audioGraph');
          graph.innerHTML = '';
          let max = Math.max(...data.samples);
          let min = Math.min(...data.samples);
          let w = graph.offsetWidth;
          let h = graph.offsetHeight;
          let ctx = graph.getContext('2d');
          ctx.clearRect(0,0,w,h);
          ctx.beginPath();
          for(let i=0;i<data.samples.length;i++){
            let x = i * w / data.samples.length;
            let y = h/2 - (data.samples[i] - min) * h/(max-min)/2;
            if(i==0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
          }
          ctx.strokeStyle = '#2196F3';
          ctx.stroke();
        });
      }
      function testPlay() {
        fetch('/test_play');
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
    <h3>Test Voice Recording</h3>
    <button class='btn' onclick='testRecord()'>Record & Show Graph</button>
    <button class='btn' onclick='testPlay()'>Play Last Recording</button>
    <button class='btn' onclick='playWebAudio()'>Play Web Audio Fragment</button><br>
    <canvas id='audioGraph' class='graph'></canvas>
    <script>
      function playWebAudio() {
        fetch('/play_web_audio');
      }
    </script>
    </body></html>
    )=====";
  // Play short web audio fragment endpoint
  server.on("/play_web_audio", HTTP_GET, []() {
    Serial.println("Play Web Audio button pressed: fetching and playing 5s fragment...");
    HTTPClient http;
    String url = "https://www2.cs.uic.edu/~i101/SoundFiles/StarWars60.wav";
    WiFiClientSecure *client = new WiFiClientSecure;
    client->setInsecure();
    http.begin(*client, url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      WiFiClient* stream = http.getStreamPtr();
      const size_t bufSize = 4096;
      uint8_t* buf = (uint8_t*)malloc(bufSize);
      if (!buf) {
        server.send(500, "text/plain", "Memory error");
        http.end();
        delete client;
        return;
      }
      size_t totalRead = 0;
      unsigned long startTime = millis();
      while (stream->available() && millis() - startTime < 5000) {
        int len = stream->read(buf, bufSize);
        if (len > 0) {
          playAudio(buf, len);
          totalRead += len;
        }
      }
      free(buf);
      Serial.printf("Played %u bytes from web audio.\n", totalRead);
      server.send(200, "text/plain", "Played web audio fragment");
    } else {
      server.send(500, "text/plain", "Failed to fetch audio");
    }
    http.end();
    delete client;
  });
    server.send(200, "text/html", html);
  });

  // Test record endpoint
  server.on("/test_record", HTTP_GET, []() {
    // Record 5 seconds of audio and return a small sample array for graph
    const int testDurationMs = 5000;
    const int sampleRate = SAMPLE_RATE;
    const int totalSamples = sampleRate * testDurationMs / 1000;
    uint8_t* testBuffer = (uint8_t*)malloc(totalSamples * 4); // 32-bit samples
    if (!testBuffer) {
      server.send(500, "application/json", "{\"error\":\"Memory error\"}");
      return;
    }
    Serial.println("Test Record button pressed: recording 5 seconds...");
    size_t bytes_read;
    i2s_read(I2S_NUM_0, testBuffer, totalSamples * 4, &bytes_read, portMAX_DELAY);
    // Downsample for graph (128 points)
    const int graphSamples = 128;
    String json = "{\"samples\": [";
    for (int i = 0; i < graphSamples; i++) {
      int idx = i * totalSamples / graphSamples;
      int32_t sample = ((int32_t*)testBuffer)[idx];
      json += String(sample);
      if (i < graphSamples - 1) json += ",";
    }
    json += "]}";
    free(testBuffer);
    server.send(200, "application/json", json);
  });

  // Test play endpoint
  server.on("/test_play", HTTP_GET, []() {
    // Play last test recording (if any)
    Serial.println("Test Play button pressed: playing last recording...");
    File file = SD.open("/recording.b64");
    if (!file) {
      server.send(404, "text/plain", "No recording found");
      return;
    }
    String b64 = file.readString();
    file.close();
    size_t decodedSize = calculateDecodedSize(b64.c_str());
    uint8_t* decodedAudio = (uint8_t*)malloc(decodedSize);
    if (!decodedAudio) {
      server.send(500, "text/plain", "Memory error");
      return;
    }
    int bytesDecoded = base64_decode(b64.c_str(), decodedAudio);
    playAudio(decodedAudio, bytesDecoded);
    free(decodedAudio);
    server.send(200, "text/plain", "Playing");
  });
  
  server.on("/save", HTTP_POST, []() {
    // Save WiFi credentials
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
      if (server.hasArg("ssid" + String(i+1))) {
        strncpy(deviceConfig.ssids[i], server.arg("ssid" + String(i+1)).c_str(), WIFI_CRED_MAX_LEN);
        Serial.printf("SSID %d changed to: %s\n", i+1, deviceConfig.ssids[i]);
      }
      if (server.hasArg("pass" + String(i+1))) {
        strncpy(deviceConfig.passwords[i], server.arg("pass" + String(i+1)).c_str(), WIFI_CRED_MAX_LEN);
        Serial.printf("Password %d changed.\n", i+1);
      }
    }
    // Save API keys
    if (server.hasArg("speech")) {
      strncpy(deviceConfig.googleSpeechApiKey, server.arg("speech").c_str(), API_KEY_LEN);
      Serial.println("Google Speech API Key changed.");
    }
    if (server.hasArg("tts")) {
      strncpy(deviceConfig.googleTtsApiKey, server.arg("tts").c_str(), API_KEY_LEN);
      Serial.println("Google TTS API Key changed.");
    }
    if (server.hasArg("gemini")) {
      strncpy(deviceConfig.geminiApiKey, server.arg("gemini").c_str(), API_KEY_LEN);
      Serial.println("Gemini API Key changed.");
    }
    saveConfig();
    server.send(200, "text/plain", "Configuration saved. Device will reboot.");
    delay(1000);
    ESP.restart();
  });
  
  // Default 404 handler for unknown requests
  server.onNotFound([]() {
    server.send(404, "text/plain", "404: Not Found");
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

void setupAudioHardware() {
  // Configure I2S for INMP441 (Microphone)
  i2s_config_t i2s_mic_config;
  i2s_mic_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2s_mic_config.sample_rate = SAMPLE_RATE;
  i2s_mic_config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2s_mic_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_mic_config.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  i2s_mic_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2s_mic_config.dma_buf_count = 8;
  i2s_mic_config.dma_buf_len = 256;

  i2s_pin_config_t mic_pins;
  mic_pins.bck_io_num = I2S_BCK;
  mic_pins.ws_io_num = I2S_WS;
  mic_pins.data_in_num = I2S_DIN;
  mic_pins.data_out_num = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_NUM_0, &i2s_mic_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &mic_pins);
  
  // Configure I2S for MAX98357 (Amplifier)
  i2s_config_t i2s_amp_config;
  i2s_amp_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2s_amp_config.sample_rate = SAMPLE_RATE;
  i2s_amp_config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2s_amp_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_amp_config.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  i2s_amp_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2s_amp_config.dma_buf_count = 8;
  i2s_amp_config.dma_buf_len = 256;

  i2s_pin_config_t amp_pins;
  amp_pins.bck_io_num = I2S_BCK;
  amp_pins.ws_io_num = I2S_WS;
  amp_pins.data_in_num = I2S_PIN_NO_CHANGE;
  amp_pins.data_out_num = I2S_DOUT;

  i2s_driver_install(I2S_NUM_1, &i2s_amp_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &amp_pins);
  
  Serial.println("Audio hardware initialized");
}

void setupSDCard() {
  if(!SD.begin(SD_CS_PIN)){
    displayStatus("SD Card Mount\nFailed");
    Serial.println("Card Mount Failed");
    delay(2000);
    return;
  }
  uint8_t cardType = SD.cardType();

  if(cardType == CARD_NONE){
    displayStatus("No SD card\nattached");
    Serial.println("No SD card attached");
    delay(2000);
    return;
  }

  String cardTypeStr;
  if(cardType == CARD_MMC){
    cardTypeStr = "MMC";
  } else if(cardType == CARD_SD){
    cardTypeStr = "SDSC";
  } else if(cardType == CARD_SDHC){
    cardTypeStr = "SDHC";
  } else {
    cardTypeStr = "UNKNOWN";
  }
  Serial.println("SD Card Type: " + cardTypeStr);
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSize);
  displayStatus("SD Card OK\n" + cardTypeStr);
  delay(1000);
}

void startRecording() {
  audioBufferSize = SAMPLE_RATE * RECORD_DURATION / 1000 * 4; // 32-bit samples
  audioBuffer = (uint8_t*)malloc(audioBufferSize);
  if (!audioBuffer) {
    setError("Memory Error");
    return;
  }
  
  memset(audioBuffer, 0, audioBufferSize);
  
  // Read initial chunk to start DMA
  size_t bytes_read;
  i2s_read(I2S_NUM_0, audioBuffer, audioBufferSize, &bytes_read, portMAX_DELAY);
  
  Serial.println("Recording started");
}

void stopRecording() {
  // Flush remaining audio data
  size_t bytes_read;
  i2s_read(I2S_NUM_0, audioBuffer, audioBufferSize, &bytes_read, 0);
  
  Serial.println("Recording stopped");
}

//========================================
// Cloud Services
//========================================

void processSpeech() {
  if (!audioBuffer || audioBufferSize == 0) {
    setError("No audio data");
    return;
  }
  
  displayStatus("Encoding audio...");
  HTTPClient http;
  http.begin("https://speech.googleapis.com/v1/speech:recognize?key=" + String(deviceConfig.googleSpeechApiKey));
  http.addHeader("Content-Type", "application/json");
  
  // Convert audio to base64
  String audioBase64 = base64_encode(audioBuffer, audioBufferSize);

  // Save to SD card
  displayStatus("Saving to SD...");
  File file = SD.open("/recording.b64", FILE_WRITE);
  if(!file){
      Serial.println("Failed to open SD file for writing.");
      displayStatus("SD Save Failed");
      delay(1000);
  } else {
      if(!file.print(audioBase64)){
          Serial.println("Write to SD file failed.");
      }
      file.close();
  }
  free(audioBuffer);
  audioBuffer = nullptr;
  
  String payload = "{\"config\":{\"encoding\":\"LINEAR16\",\"sampleRateHertz\":" + String(SAMPLE_RATE) + 
                   ",\"languageCode\":\"en-US\"},\"audio\":{\"content\":\"" + audioBase64 + "\"}}";
  
  displayStatus("Processing speech...");
  int httpCode = http.POST(payload);
  
  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    JsonDocument doc;
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
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error && doc["candidates"].is<JsonArray>()) {
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
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, response);
        
        if (!error && doc["audioContent"].is<const char*>()) {
            const char* audioContent = doc["audioContent"];
            size_t decodedSize = calculateDecodedSize(audioContent);
            uint8_t* decodedAudio = (uint8_t*)malloc(decodedSize);
            
            // Use the custom base64 decode function
            int bytesDecoded = base64_decode(audioContent, decodedAudio);
            
            displayStatus("Playing response...");
            currentState = STATE_PLAYING;
            playAudio(decodedAudio, bytesDecoded);
            
            free(decodedAudio);
        } else if (error) {
            setError("JSON Parse Err: " + String(error.c_str()));
        }
    } else {
        setError("TTS API: " + String(httpCode));
    }
    
    http.end();
}

void playAudio(const uint8_t* audioData, size_t size) {
  isPlayingAudio = true;
  
  // Simple streaming playback (would need proper buffering for large audio)
  size_t bytes_written;
  i2s_write(I2S_NUM_1, audioData , size, &bytes_written, portMAX_DELAY);
  
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
