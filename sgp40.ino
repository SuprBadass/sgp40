#include "SparkFun_SGP40_Arduino_Library.h" // Click here to get the library: http://librarymanager/All#SparkFun_SGP40
#include <Wire.h>
#include <EEPROM.h> // ESP32 core emulates EEPROM in flash; API matches classic AVR EEPROM

// The SparkFun SGP40 class keeps its own VocAlgorithmParams as a private
// member, so getVOCindex() can't be combined with VocAlgorithm_get_states()/
// set_states() from a sketch. Instead we run the Sensirion algorithm
// ourselves: pull raw ticks with measureRaw() and feed them into our own
// VocAlgorithmParams via VocAlgorithm_process(). sensirion_voc_algorithm.h
// ships inside the SparkFun library, so it's already on the include path.
extern "C" {
#include "sensirion_voc_algorithm.h"
}

SGP40 mySensor; //create an object of the SGP40 class
VocAlgorithmParams vocParams;

const uint32_t SAMPLE_INTERVAL_MS = 1000UL;                // Sensirion algorithm expects ~1 Hz samples
const uint32_t SAVE_INTERVAL_MS = 4UL * 60UL * 60UL * 1000UL; // 4 hours
const int EEPROM_SIZE = 16; // rounds up to a flash sector internally, plenty for our 12-byte struct
const int EEPROM_ADDR = 0;
const uint32_t STATE_MAGIC = 0x53475034; // "SGP4" - bump this if VocState's layout ever changes

struct VocState
{
  uint32_t magic;
  int32_t state0;
  int32_t state1;
};

unsigned long lastSampleMs = 0;
unsigned long lastSaveMs = 0;

void loadVocState()
{
  VocState saved;
  EEPROM.get(EEPROM_ADDR, saved);

  if (saved.magic == STATE_MAGIC)
  {
    VocAlgorithm_set_states(&vocParams, saved.state0, saved.state1);
    Serial.println(F("Restored VOC algorithm state from EEPROM"));
  }
  else
  {
    Serial.println(F("No saved VOC state found in EEPROM, starting fresh"));
  }
}

void saveVocState()
{
  VocState toSave;
  toSave.magic = STATE_MAGIC;
  VocAlgorithm_get_states(&vocParams, &toSave.state0, &toSave.state1);

  EEPROM.put(EEPROM_ADDR, toSave);
  EEPROM.commit(); // required on ESP32 - writes are buffered in RAM until this is called

  Serial.print(F("Saved VOC algorithm state (state0="));
  Serial.print(toSave.state0);
  Serial.print(F(", state1="));
  Serial.print(toSave.state1);
  Serial.println(F(")"));
}

void setup()
{
  Serial.begin(115200);
  Serial.println(F("SGP40 Example - EEPROM state persistence"));

  Wire.begin();

  if (EEPROM.begin(EEPROM_SIZE) == false)
  {
    Serial.println(F("Failed to initialize EEPROM. Freezing..."));
    while (1)
      ;
  }

  //mySensor.enableDebugging(); // Uncomment this line to print useful debug messages to Serial

  //Initialize sensor
  if (mySensor.begin() == false)
  {
    Serial.println(F("SGP40 not detected. Check connections. Freezing..."));
    while (1)
      ; // Do nothing more
  }

  VocAlgorithm_init(&vocParams);
  loadVocState(); // overwrites the fresh init state if we have a saved one

  lastSampleMs = millis();
  lastSaveMs = millis();
}

void loop()
{
  unsigned long now = millis();

  // Sample at ~1 Hz, as the Sensirion algorithm expects
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS)
  {
    lastSampleMs = now;

    uint16_t srawTicks;
    if (mySensor.measureRaw(&srawTicks) == SGP40_SUCCESS) //default RH (50%) and T (25C)
    {
      int32_t vocIndex;
      VocAlgorithm_process(&vocParams, srawTicks, &vocIndex);
      Serial.print(F("VOC Index is: "));
      Serial.println(vocIndex);
    }
    else
    {
      Serial.println(F("measureRaw() failed"));
    }
  }

  // Persist algorithm state every 4 hours so it survives a reboot/power loss
  if (now - lastSaveMs >= SAVE_INTERVAL_MS)
  {
    lastSaveMs = now;
    saveVocState();
  }
}
