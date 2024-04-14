/* 
 
Arduino Nano BLE Sense dashboard demo


Hardware required: https://store.arduino.cc/nano-33-ble-sense

1) Upload this sketch to the Arduino Nano BLE sense board 

2) Open the following web page in the Chrome browser:
https://arduino.github.io/ArduinoAI/BLESense-test-dashboard/

3) Click on the green button in the web page to connect the browser to the board over BLE


Web dashboard by D Pajak

Device sketch based on example by Sandeep Mistry

*/

#include <Arduino_LSM9DS1.h>

#include <ArduinoBLE.h>

#define BLE_SENSE_UUID(val) ("4798e0f2-" val "-4d68-af64-8a8f5258404e")

const int VERSION = 0x00000000;

constexpr int stroke_transmit_stride = 2;
constexpr int stroke_transmit_max_length = 160;
constexpr int stroke_max_length = stroke_transmit_max_length * stroke_transmit_stride;
constexpr int stroke_points_byte_count = 2 * sizeof(int8_t) * stroke_transmit_max_length;
constexpr int stroke_struct_byte_count = (2 * sizeof(int32_t)) + stroke_points_byte_count;
constexpr int moving_sample_count = 50;

BLEService        service                       (BLE_SENSE_UUID("0000"));
BLECharacteristic strokeCharacteristic          (BLE_SENSE_UUID("300a"), BLERead, stroke_struct_byte_count);

// String to calculate the local and device name
String name;

// A buffer holding the last 600 sets of 3-channel values from the accelerometer.
constexpr int acceleration_data_length = 600 * 3;
float acceleration_data[acceleration_data_length] = {};
// The next free entry in the data array.
int acceleration_data_index = 0;
float acceleration_sample_rate = 0.0f;

// A buffer holding the last 600 sets of 3-channel values from the gyroscope.
constexpr int gyroscope_data_length = 600 * 3;
float gyroscope_data[gyroscope_data_length] = {};
float orientation_data[gyroscope_data_length] = {};
// The next free entry in the data array.
int gyroscope_data_index = 0;
float gyroscope_sample_rate = 0.0f;

float current_velocity[3] = {0.0f, 0.0f, 0.0f};
float current_position[3] = {0.0f, 0.0f, 0.0f};
float current_gravity[3] = {0.0f, 0.0f, 0.0f};
float current_gyroscope_drift[3] = {0.0f, 0.0f, 0.0f};

int32_t stroke_length = 0;
uint8_t stroke_struct_buffer[stroke_struct_byte_count] = {};
int32_t* stroke_state = reinterpret_cast<int32_t*>(stroke_struct_buffer);
int32_t* stroke_transmit_length = reinterpret_cast<int32_t*>(stroke_struct_buffer + sizeof(int32_t));
int8_t* stroke_points = reinterpret_cast<int8_t*>(stroke_struct_buffer + (sizeof(int32_t) * 2));


const int buttonPin = 2;  // the number of the pushbutton pin
int buttonState = 0;  // variable for reading the pushbutton status
int GestureState = 0;

enum { 
  eWaiting = 0,
  eDrawing = 1,
  eDone = 2,
};

void SetupIMU() {

  // Make sure we are pulling measurements into a FIFO.
  // If you see an error on this line, make sure you have at least v1.1.0 of the
  // Arduino_LSM9DS1 library installed.
  IMU.setContinuousMode();

  acceleration_sample_rate = IMU.accelerationSampleRate();
  gyroscope_sample_rate = IMU.gyroscopeSampleRate();
}

int ReadAccelerometerAndGyroscope(int* new_accelerometer_samples, int* new_gyroscope_samples) {
  // Keep track of whether we stored any new data
  *new_accelerometer_samples = 0;
  *new_gyroscope_samples = 0;
  // Loop through new samples and add to buffer
  while (IMU.accelerationAvailable()) {
    const int gyroscope_index = (gyroscope_data_index % gyroscope_data_length);
    gyroscope_data_index += 3;
    float* current_gyroscope_data = &gyroscope_data[gyroscope_index];
    // Read each sample, removing it from the device's FIFO buffer
    if (!IMU.readGyroscope(
        current_gyroscope_data[0], current_gyroscope_data[1], current_gyroscope_data[2])) {
      Serial.println("Failed to read gyroscope data");
      break;
    }
    *new_gyroscope_samples += 1;

    const int acceleration_index = (acceleration_data_index % acceleration_data_length);
    acceleration_data_index += 3;
    float* current_acceleration_data = &acceleration_data[acceleration_index];
    // Read each sample, removing it from the device's FIFO buffer
    if (!IMU.readAcceleration(
        current_acceleration_data[0], current_acceleration_data[1], current_acceleration_data[2])) {
      Serial.println("Failed to read acceleration data");
      break;
    }
    *new_accelerometer_samples += 1;
  }
}

int ReadGyroscope() {
  // Keep track of whether we stored any new data
  int new_samples = 0;
  // Loop through new samples and add to buffer
  while (IMU.gyroscopeAvailable()) {
    const int index = (gyroscope_data_index % gyroscope_data_length);
    gyroscope_data_index += 3;
    float* data = &gyroscope_data[index];
    // Read each sample, removing it from the device's FIFO buffer
    if (!IMU.readGyroscope(data[0], data[1], data[2])) {
      Serial.println("Failed to read gyroscope data");
      break;
    }
    new_samples += 1;
  }
  return new_samples;
}

float VectorMagnitude(const float* vec) {
  const float x = vec[0];
  const float y = vec[1];
  const float z = vec[2];
  return sqrtf((x * x) + (y * y) + (z * z));
}

void NormalizeVector(const float* in_vec, float* out_vec) {
  const float magnitude = VectorMagnitude(in_vec);
  const float x = in_vec[0];
  const float y = in_vec[1];
  const float z = in_vec[2];
  out_vec[0] = x / magnitude;
  out_vec[1] = y / magnitude;
  out_vec[2] = z / magnitude;
}

float DotProduct(const float* a, const float* b) {
  return (a[0] * b[0], a[1] * b[1], a[2] * b[2]);
}

void EstimateGravityDirection(float* gravity) {
  int samples_to_average = 100;
  if (samples_to_average >= acceleration_data_index) {
    samples_to_average = acceleration_data_index;
  }

  const int start_index = ((acceleration_data_index + 
    (acceleration_data_length - (3 * (samples_to_average + 1)))) % 
    acceleration_data_length);

  float x_total = 0.0f;
  float y_total = 0.0f;
  float z_total = 0.0f;
  for (int i = 0; i < samples_to_average; ++i) {
    const int index = ((start_index + (i * 3)) % acceleration_data_length);
    const float* entry = &acceleration_data[index];
    const float x = entry[0];
    const float y = entry[1];
    const float z = entry[2];
    x_total += x;
    y_total += y;
    z_total += z;
  }
  gravity[0] = x_total / samples_to_average;
  gravity[1] = y_total / samples_to_average;
  gravity[2] = z_total / samples_to_average;
}

void UpdateVelocity(int new_samples, float* gravity) {
  const float gravity_x = gravity[0];
  const float gravity_y = gravity[1];
  const float gravity_z = gravity[2];

  const int start_index = ((acceleration_data_index + 
    (acceleration_data_length - (3 * (new_samples + 1)))) % 
    acceleration_data_length);

  const float friction_fudge = 0.98f;

  for (int i = 0; i < new_samples; ++i) {
    const int index = ((start_index + (i * 3)) % acceleration_data_length);
    const float* entry = &acceleration_data[index];
    const float ax = entry[0];
    const float ay = entry[1];
    const float az = entry[2];
    
    // Try to remove gravity from the raw acceleration values.
    const float ax_minus_gravity = ax - gravity_x;
    const float ay_minus_gravity = ay - gravity_y;
    const float az_minus_gravity = az - gravity_z;

    // Update velocity based on the normalized acceleration.
    current_velocity[0] += ax_minus_gravity;
    current_velocity[1] += ay_minus_gravity;
    current_velocity[2] += az_minus_gravity;
    
    // Dampen the velocity slightly with a fudge factor to stop it exploding.
    current_velocity[0] *= friction_fudge;
    current_velocity[1] *= friction_fudge;
    current_velocity[2] *= friction_fudge;
    
    // Update the position estimate based on the velocity.
    current_position[0] += current_velocity[0];
    current_position[1] += current_velocity[1];
    current_position[2] += current_velocity[2];
  }
}

void EstimateGyroscopeDrift(float* drift) {
  const bool isMoving = VectorMagnitude(current_velocity) > 0.1f;
  if (isMoving) {
    return;
  }
  
  int samples_to_average = 20;
  if (samples_to_average >= gyroscope_data_index) {
    samples_to_average = gyroscope_data_index;
  }

  const int start_index = ((gyroscope_data_index + 
    (gyroscope_data_length - (3 * (samples_to_average + 1)))) % 
    gyroscope_data_length);

  float x_total = 0.0f;
  float y_total = 0.0f;
  float z_total = 0.0f;
  for (int i = 0; i < samples_to_average; ++i) {
    const int index = ((start_index + (i * 3)) % gyroscope_data_length);
    const float* entry = &gyroscope_data[index];
    const float x = entry[0];
    const float y = entry[1];
    const float z = entry[2];
    x_total += x;
    y_total += y;
    z_total += z;
  }
  drift[0] = x_total / samples_to_average;
  drift[1] = y_total / samples_to_average;
  drift[2] = z_total / samples_to_average;
}

void UpdateOrientation(int new_samples, float* gravity, float* drift) {
  const float drift_x = drift[0];
  const float drift_y = drift[1];
  const float drift_z = drift[2];

  const int start_index = ((gyroscope_data_index + 
    (gyroscope_data_length - (3 * new_samples))) % 
    gyroscope_data_length);

  // The gyroscope values are in degrees-per-second, so to approximate
  // degrees in the integrated orientation, we need to divide each value
  // by the number of samples each second.
  const float recip_sample_rate = 1.0f / gyroscope_sample_rate;

  for (int i = 0; i < new_samples; ++i) {
    const int index = ((start_index + (i * 3)) % gyroscope_data_length);
    const float* entry = &gyroscope_data[index];
    const float dx = entry[0];
    const float dy = entry[1];
    const float dz = entry[2];
    
    // Try to remove sensor errors from the raw gyroscope values.
    const float dx_minus_drift = dx - drift_x;
    const float dy_minus_drift = dy - drift_y;
    const float dz_minus_drift = dz - drift_z;

    // Convert from degrees-per-second to appropriate units for this
    // time interval.
    const float dx_normalized = dx_minus_drift * recip_sample_rate;
    const float dy_normalized = dy_minus_drift * recip_sample_rate;
    const float dz_normalized = dz_minus_drift * recip_sample_rate;

    // Update orientation based on the gyroscope data.
    float* current_orientation = &orientation_data[index];
    const int previous_index = (index + (gyroscope_data_length - 3)) % gyroscope_data_length;
    const float* previous_orientation = &orientation_data[previous_index];
    current_orientation[0] = previous_orientation[0] + dx_normalized;
    current_orientation[1] = previous_orientation[1] + dy_normalized;
    current_orientation[2] = previous_orientation[2] + dz_normalized;
  }
}

bool IsMoving(int samples_before) {
  constexpr float moving_threshold = 10.0f;
  
  if ((gyroscope_data_index - samples_before) < moving_sample_count) {
    return false;
  }

  const int start_index = ((gyroscope_data_index + 
    (gyroscope_data_length - (3 * (moving_sample_count + samples_before)))) % 
    gyroscope_data_length);

  float total = 0.0f;
  for (int i = 0; i < moving_sample_count; ++i) {
    const int index = ((start_index + (i * 3)) % gyroscope_data_length);
    float* current_orientation = &orientation_data[index];
    const int previous_index = (index + (gyroscope_data_length - 3)) % gyroscope_data_length;
    const float* previous_orientation = &orientation_data[previous_index];
    const float dx = current_orientation[0] - previous_orientation[0];
    const float dy = current_orientation[1] - previous_orientation[1];
    const float dz = current_orientation[2] - previous_orientation[2];
    const float mag_squared = (dx * dx) + (dy * dy) + (dz * dz);
    total += mag_squared;
  }
  const bool is_moving = (total > moving_threshold);
  return is_moving;
}

void UpdateStroke(int new_samples) {
  constexpr int minimum_stroke_length = moving_sample_count + 10;
  constexpr float minimum_stroke_size = 0.2f;

  for (int i = 0; i < new_samples; ++i) {
    const int current_head = (new_samples - (i + 1));
    const bool is_moving = IsMoving(current_head);
    const int32_t old_state = *stroke_state;
    if ((old_state == eWaiting) || (old_state == eDone)) {
      if (is_moving) {
        stroke_length = moving_sample_count;
        *stroke_state = eDrawing;
      }
    } else if (old_state == eDrawing) {
      if (!is_moving) {
        if (stroke_length > minimum_stroke_length) {
          *stroke_state = eDone;
        } else {
          stroke_length = 0;
          *stroke_state = eWaiting;
        }
      }
    }
  
    const bool is_waiting = (*stroke_state == eWaiting);
    if (is_waiting) {
      continue;
    }
    
    stroke_length += 1;
    if (stroke_length > stroke_max_length) {
      stroke_length = stroke_max_length;
    }
  
    // Only recalculate the full stroke if it's needed.
    const bool draw_last_point = ((i == (new_samples -1)) && (*stroke_state == eDrawing));
    const bool done_just_triggered = ((old_state != eDone) && (*stroke_state == eDone));
    if (!(done_just_triggered || draw_last_point)) {
      continue;
    }

    const int start_index = ((gyroscope_data_index + 
      (gyroscope_data_length - (3 * (stroke_length + current_head)))) % 
      gyroscope_data_length);

    float x_total = 0.0f;
    float y_total = 0.0f;
    float z_total = 0.0f;
    for (int j = 0; j < stroke_length; ++j) {
      const int index = ((start_index + (j * 3)) % gyroscope_data_length);
      const float* entry = &orientation_data[index];  
      x_total += entry[0];
      y_total += entry[1];
      z_total += entry[2];
    }
  
    const float x_mean = x_total / stroke_length;
    const float y_mean = y_total / stroke_length;
    const float z_mean = z_total / stroke_length;
    constexpr float range = 90.0f;

    const float gy = current_gravity[1];
    const float gz = current_gravity[2];
    float gmag = sqrtf((gy * gy) + (gz * gz));
    if (gmag < 0.0001f) {
      gmag = 0.0001f;
    }
    const float ngy = gy / gmag;
    const float ngz = gz / gmag;
  
    const float xaxisz = -ngz;
    const float xaxisy = -ngy;

    const float yaxisz = -ngy;
    const float yaxisy = ngz;
    
    *stroke_transmit_length = stroke_length / stroke_transmit_stride;

    float x_min;
    float y_min;
    float x_max;
    float y_max;
    for (int j = 0; j < *stroke_transmit_length; ++j) {
      const int orientation_index = ((start_index + ((j * stroke_transmit_stride) * 3)) % gyroscope_data_length);
      const float* orientation_entry = &orientation_data[orientation_index];  

      const float orientation_x = orientation_entry[0];
      const float orientation_y = orientation_entry[1];
      const float orientation_z = orientation_entry[2];

      const float nx = (orientation_x - x_mean) / range;
      const float ny = (orientation_y - y_mean) / range;
      const float nz = (orientation_z - z_mean) / range;
    
      const float x_axis = (xaxisz * nz) + (xaxisy * ny);
      const float y_axis = (yaxisz * nz) + (yaxisy * ny);    

      const int stroke_index = j * 2;
      int8_t* stroke_entry = &stroke_points[stroke_index];
      
      int32_t unchecked_x = static_cast<int32_t>(roundf(x_axis * 128.0f));
      int8_t stored_x;
      if (unchecked_x > 127) {
        stored_x = 127;
      } else if (unchecked_x < -128) {
        stored_x = -128;
      } else {
        stored_x = unchecked_x;
      }
      stroke_entry[0] = stored_x;
      
      int32_t unchecked_y = static_cast<int32_t>(roundf(y_axis * 128.0f));
      int8_t stored_y;
      if (unchecked_y > 127) {
        stored_y = 127;
      } else if (unchecked_y < -128) {
        stored_y = -128;
      } else {
        stored_y = unchecked_y;
      }
      stroke_entry[1] = stored_y;

      const bool is_first = (j == 0);
      if (is_first || (x_axis < x_min)) {
        x_min = x_axis;
      }
      if (is_first || (y_axis < y_min)) {
        y_min = y_axis;
      }
      if (is_first || (x_axis > x_max)) {
        x_max = x_axis;
      }
      if (is_first || (y_axis > y_max)) {
        y_max = y_axis;
      }
    }
    
    // If the stroke is too small, cancel it.
    if (done_just_triggered) {
      const float x_range = (x_max - x_min);
      const float y_range = (y_max - y_min);
      if ((x_range < minimum_stroke_size) &&
        (y_range < minimum_stroke_size)) {
        *stroke_state = eWaiting;
        *stroke_transmit_length = 0;
        stroke_length = 0;
      }
    }
  }
}

void setup() {
  Serial.begin(9600);

  //while (!Serial);
  Serial.println("Started");

  if (!IMU.begin()) {
    Serial.println("Failed to initialized IMU!");
    while (1);
  }
  
  SetupIMU();

  if (!BLE.begin()) {
    Serial.println("Failed to initialized BLE!");
    while (1);
  }

  String address = BLE.address();

  Serial.print("address = ");
  Serial.println(address);

  address.toUpperCase();

  name = "BLESense-";
  name += address[address.length() - 5];
  name += address[address.length() - 4];
  name += address[address.length() - 2];
  name += address[address.length() - 1];

  Serial.print("name = ");
  Serial.println(name);

  BLE.setLocalName(name.c_str());
  BLE.setDeviceName(name.c_str());
  BLE.setAdvertisedService(service);

  service.addCharacteristic(strokeCharacteristic);

  BLE.addService(service);

  BLE.advertise();
}

void loop() {
  BLEDevice central = BLE.central();
  
    // if a central is connected to the peripheral:
  if (central) {
    Serial.print("Connected to central: ");
    // print the central's BT address:
    Serial.println(central.address());
  }
  
  while (central && central.connected()) {
    const bool data_available = IMU.accelerationAvailable() || IMU.gyroscopeAvailable();
    if (!data_available) {
      continue;
    }

    buttonState = digitalRead(buttonPin);
    
    if(buttonState == 1){
      GestureState = 1;
      Serial.println("Button Pushed!");
    }

    if (GestureState == 1) {
      int accelerometer_samples_read;
      int gyroscope_samples_read;
      ReadAccelerometerAndGyroscope(&accelerometer_samples_read, &gyroscope_samples_read);

      if (gyroscope_samples_read > 0) {
        EstimateGyroscopeDrift(current_gyroscope_drift);
        UpdateOrientation(gyroscope_samples_read, current_gravity, current_gyroscope_drift);
        UpdateStroke(gyroscope_samples_read);
        strokeCharacteristic.writeValue(stroke_struct_buffer, stroke_struct_byte_count); //send data to central 
      }

      if (accelerometer_samples_read > 0) {
        EstimateGravityDirection(current_gravity);
        UpdateVelocity(accelerometer_samples_read, current_gravity);
      }
    }
    else{
      Serial.println("Waiting");
    }
    GestureState = 0;

    
  }
}

